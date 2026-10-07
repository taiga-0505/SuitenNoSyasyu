#include "ModelMesh.h"
#include "Common/ResourcePath.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <chrono>
#include <format>
#include "Common/Log/Log.h"

#if __has_include(<assimp/version.h>)
#include <assimp/version.h>
#endif

using namespace RC;
namespace fs = std::filesystem;

static constexpr float kPi = std::numbers::pi_v<float>;

ModelMesh::~ModelMesh() {
  if (vb_.resource) {
    vb_.resource.Reset();
  }
}

bool ModelMesh::LoadModel(ID3D12Device *device, const std::string &modelPath) {
  device_ = device;

  auto IsSupported = [](std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".obj" || ext == ".gltf" || ext == ".glb";
  };

  // ゲーム側 Resources → エンジン側 (Chaso/Resources) の順で探す
  fs::path p = Chaso::ResolvePath(fs::path(modelPath));
  fs::path file;

  // 毎回クリア（前回の表示が残らないように）
  sourceInputPath_.clear();
  sourceFilePath_.clear();

  if (fs::is_regular_file(p)) {
    if (!IsSupported(p.extension().string())) {
      return false;
    }
    file = p;

  } else if (fs::is_directory(p)) {
    // 優先順：gltf -> glb -> obj
    const std::vector<std::string> exts = {".gltf", ".glb", ".obj"};
    for (auto &extWant : exts) {
      for (auto &entry : fs::directory_iterator(p)) {
        if (!entry.is_regular_file())
          continue;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == extWant) {
          file = entry.path();
          break;
        }
      }
      if (!file.empty())
        break;
    }
    if (file.empty()) {
      return false;
    }

  } else {
    return false;
  }

  sourceInputPath_ = modelPath;
  sourceFilePath_ = file.lexically_normal().string();

  // LoadAssimp_ もこの相対パスで呼ぶ
  if (!LoadAssimp_(sourceFilePath_)) {
    sourceFilePath_.clear();
    return false;
  }

  EnsureSphericalUVIfMissing();
  return true;
}


bool ModelMesh::LoadAssimp_(const std::string &filePath) {
  Assimp::Importer importer;
  const unsigned int flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_FlipWindingOrder | aiProcess_MakeLeftHanded;

  auto start = std::chrono::high_resolution_clock::now();
  const aiScene *scene = importer.ReadFile(filePath.c_str(), flags);
  auto end = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[ModelMesh] Assimp ReadFile: {:.3f}ms, Path: {}", std::chrono::duration<float, std::milli>(end - start).count(), filePath));

  if (!scene || !scene->HasMeshes()) {
    return false;
  }

  // ---------------------------------------------------------
  // Mesh / Material / Node を読み出す
  // ---------------------------------------------------------
  std::vector<VertexData> verts;
  const std::string baseDir = fs::path(filePath).parent_path().string();

  materials_.clear();
  submeshes_.clear();
  drawItems_.clear();
  indices_.clear();
  skinData_ = {};
  materialFile_ = {};

  start = std::chrono::high_resolution_clock::now();
  if (!ExtractScene_(scene, baseDir, verts)) {
    return false;
  }
  end = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[ModelMesh] ExtractScene: {:.3f}ms", std::chrono::duration<float, std::milli>(end - start).count()));

  // スキンデータ抽出（ボーンがある場合）
  ExtractSkinData_(scene, verts);

  // RootNode（階層）
  rootNode_ = ReadNode_(scene->mRootNode);

  // DrawItem（Nodeの行列を累積して Mesh範囲と結びつける）
  drawItems_.clear();
  BuildDrawItems_(rootNode_, MakeIdentity4x4());

  Log::Print(std::format("[ModelMesh] DrawItems: {}, Vertices: {}, Indices: {}, Submeshes: {}, Materials: {}, HasSkin: {}, Path: {}",
      drawItems_.size(), verts.size(), indices_.size(), submeshes_.size(), materials_.size(), HasSkinData(), filePath));

  // カリング用の境界箱（VB 作成＝Ready() になる前に確定させておく）
  ComputeBounds_(verts);

  start = std::chrono::high_resolution_clock::now();
  // VBアップロード
  UploadVB_(verts);
  // IBアップロード
  UploadIB_();
  end = std::chrono::high_resolution_clock::now();
  Log::Print(std::format("[ModelMesh] UploadVB+IB: {:.3f}ms", std::chrono::duration<float, std::milli>(end - start).count()));
  return true;
}

bool ModelMesh::ExtractScene_(const aiScene *scene, const std::string &baseDir,
                              std::vector<VertexData> &outVertices) {
  if (!scene)
    return false;

  // ---------------------------------------------------------
  // Material: texturePath を取得
  // ---------------------------------------------------------
  materials_.resize(scene->mNumMaterials);
  for (uint32_t i = 0; i < scene->mNumMaterials; ++i) {
    const aiMaterial *mat = scene->mMaterials[i];
    MaterialData md{};
    md.textureFilePath = ResolveTexturePath_(scene, mat, baseDir);
    materials_[i] = md;

    // 互換：最初の1枚だけ MaterialFile() で返す
    if (materialFile_.textureFilePath.empty() && !md.textureFilePath.empty()) {
      materialFile_ = md;
    }
  }

  // ---------------------------------------------------------
  // Mesh: 全meshを1本のVBに連結し、meshIndex->範囲 を記録
  //       IndexBuffer方式: 頂点をそのままコピーし、Faceからインデックスを抽出
  // ---------------------------------------------------------
  submeshes_.resize(scene->mNumMeshes);

  outVertices.clear();
  outVertices.reserve(65536);
  indices_.clear();
  indices_.reserve(65536);

  for (uint32_t meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
    const aiMesh *mesh = scene->mMeshes[meshIndex];
    if (!mesh)
      continue;

    const uint32_t vtxStart = static_cast<uint32_t>(outVertices.size());
    const uint32_t idxStart = static_cast<uint32_t>(indices_.size());
    const bool hasNormals = mesh->HasNormals();
    const bool hasUV0 = mesh->HasTextureCoords(0);

    // 頂点をそのままコピー（Face展開しない）
    for (uint32_t vi = 0; vi < mesh->mNumVertices; ++vi) {
      const aiVector3D &p = mesh->mVertices[vi];
      const aiVector3D n =
          hasNormals ? mesh->mNormals[vi] : aiVector3D(0, 1, 0);
      const aiVector3D t =
          hasUV0 ? mesh->mTextureCoords[0][vi] : aiVector3D(0, 0, 0);

      VertexData v{};
      // aiProcess_MakeLeftHanded により Assimp が座標系変換済み
      v.position = {p.x, p.y, p.z, 1.0f};
      v.normal = {n.x, n.y, n.z};
      // UVは aiProcess_FlipUVs に任せる
      v.texcoord = {t.x, t.y};
      outVertices.push_back(v);
    }

    // Faceからインデックスを抽出
    for (uint32_t faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
      const aiFace &face = mesh->mFaces[faceIndex];
      if (face.mNumIndices != 3) {
        continue; // Triangulate済み想定だが保険
      }

      for (uint32_t k = 0; k < 3; ++k) {
        indices_.push_back(vtxStart + face.mIndices[k]);
      }
    }

    const uint32_t vtxCount = static_cast<uint32_t>(mesh->mNumVertices);
    const uint32_t idxCount = static_cast<uint32_t>(indices_.size()) - idxStart;

    SubmeshRange r{};
    r.vertexStart = vtxStart;
    r.vertexCount = vtxCount;
    r.indexStart = idxStart;
    r.indexCount = idxCount;
    r.materialIndex = mesh->mMaterialIndex;
    // 念のため範囲外は 0 に丸める（壊れたデータ対策）
    if (!materials_.empty() && r.materialIndex >= materials_.size()) {
      r.materialIndex = 0;
    }

    submeshes_[meshIndex] = r;
  }

  // 使っているMaterialだけに詰めて、indexを 0..N-1 に揃える
  CompactMaterials_();

  return !outVertices.empty();
}

Node ModelMesh::ReadNode_(const aiNode *node) const {
  Node result{};
  if (!node) {
    result.localMatrix = MakeIdentity4x4();
    result.transform = {{1,1,1}, {0,0,0,1}, {0,0,0}};
    return result;
  }

  // ---------------------------------------------------------
  // 1. localMatrix: Transpose + ConvertNodeMatrixRHtoLH_
  //    非スキンモデルのBuildDrawItems_で使われる。
  // ---------------------------------------------------------
  aiMatrix4x4 a = node->mTransformation;
  a.Transpose();

  RC::Matrix4x4 m = MakeIdentity4x4();
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      m.m[r][c] = a[r][c];
    }
  }
  // aiProcess_MakeLeftHanded により Assimp が座標系変換済みのため手動変換不要
  result.localMatrix = m;

  // ---------------------------------------------------------
  // 2. transform: aiNode::Decompose で SRT を抽出
  //    Skeleton/スキニング用のSRTデータ。
  //    Assimpの標準機能を使用して安全にSRTを分解する。
  // ---------------------------------------------------------
  {
    aiVector3D aiScale, aiTranslate;
    aiQuaternion aiRotate;
    node->mTransformation.Decompose(aiScale, aiRotate, aiTranslate);

    // aiProcess_MakeLeftHanded により座標系変換済み
    result.transform.scale     = { aiScale.x, aiScale.y, aiScale.z };
    result.transform.rotate    = { aiRotate.x, aiRotate.y, aiRotate.z, aiRotate.w };
    result.transform.translate = { aiTranslate.x, aiTranslate.y, aiTranslate.z };
  }

  result.name = node->mName.C_Str();

  // このNodeが参照しているMeshのindex配列
  result.meshIndices.resize(node->mNumMeshes);
  for (uint32_t i = 0; i < node->mNumMeshes; ++i) {
    result.meshIndices[i] = node->mMeshes[i];
  }

  // 子ノード（再帰）
  result.children.resize(node->mNumChildren);
  for (uint32_t ci = 0; ci < node->mNumChildren; ++ci) {
    result.children[ci] = ReadNode_(node->mChildren[ci]);
  }

  return result;
}

void ModelMesh::BuildDrawItems_(const Node &node,
                                const RC::Matrix4x4 &parentWorld) {
  // Nodeの累積：childWorld = local * parentWorld
  const RC::Matrix4x4 nodeWorld = Multiply(node.localMatrix, parentWorld);

  // nodeが参照しているmeshを、VB/IB範囲に変換して描画単位にする
  for (uint32_t mi : node.meshIndices) {
    if (mi >= submeshes_.size())
      continue;

    const SubmeshRange &r = submeshes_[mi];
    if (r.vertexCount == 0 && r.indexCount == 0)
      continue;

    DrawItem item{};
    item.vertexStart = r.vertexStart;
    item.vertexCount = r.vertexCount;
    item.indexStart = r.indexStart;
    item.indexCount = r.indexCount;
    item.materialIndex = r.materialIndex;
    item.meshIndex = mi;
    item.nodeWorld = nodeWorld;
    item.nodeName = node.name;
    drawItems_.push_back(std::move(item));
  }

  // 子へ
  for (const auto &c : node.children) {
    BuildDrawItems_(c, nodeWorld);
  }
}

void ModelMesh::EnsureSphericalUVIfMissing() {
  if (!vb_.resource || vb_.vertexCount == 0)
    return;

  VertexData *vtx = nullptr;
  vb_.resource->Map(0, nullptr, reinterpret_cast<void **>(&vtx));
  if (!vtx)
    return;

  // (0,0)しか無いなら焼く
  bool allZero = true;
  for (uint32_t i = 0; i < vb_.vertexCount; ++i) {
    if (std::fabs(vtx[i].texcoord.x) > 1e-6f ||
        std::fabs(vtx[i].texcoord.y) > 1e-6f) {
      allZero = false;
      break;
    }
  }
  if (!allZero) {
    vb_.resource->Unmap(0, nullptr);
    return;
  }

  // 球面UV（ざっくり）
  for (uint32_t i = 0; i < vb_.vertexCount; ++i) {
    const Vector3 p{vtx[i].position.x, vtx[i].position.y, vtx[i].position.z};
    const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    Vector3 n = (len > 1e-6f) ? Vector3{p.x / len, p.y / len, p.z / len}
                              : Vector3{0, 1, 0};

    float u = 0.5f + std::atan2(n.z, n.x) / (2.0f * kPi);
    float v = 0.5f - std::asin(n.y) / kPi;
    vtx[i].texcoord = {u, v};
  }

  vb_.resource->Unmap(0, nullptr);
}

void ModelMesh::ComputeBounds_(const std::vector<VertexData> &vertices) {
  auto isIdentity = [](const RC::Matrix4x4 &m) {
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        const float expect = (r == c) ? 1.0f : 0.0f;
        if (std::abs(m.m[r][c] - expect) > 1e-5f) return false;
      }
    }
    return true;
  };

  RC::BoundingBox total = RC::BoundingBox::Empty();
  allNodeWorldIdentity_ = true;

  auto rangeBounds = [&](uint32_t start, uint32_t count) {
    RC::BoundingBox b = RC::BoundingBox::Empty();
    const uint32_t end = (std::min)(static_cast<uint32_t>(vertices.size()), start + count);
    for (uint32_t i = start; i < end; ++i) {
      const auto &p = vertices[i].position;
      b.Encapsulate({p.x, p.y, p.z});
    }
    return b;
  };

  if (drawItems_.empty()) {
    total = rangeBounds(0, static_cast<uint32_t>(vertices.size()));
  } else {
    for (const auto &it : drawItems_) {
      if (!isIdentity(it.nodeWorld)) allNodeWorldIdentity_ = false;
      const RC::BoundingBox b = rangeBounds(it.vertexStart, it.vertexCount);
      if (!b.IsValid()) continue;
      // ノード行列で回した 8 頂点を包む（保守的）
      const RC::BoundingBox w = b.Transformed(it.nodeWorld);
      total = RC::BoundingBox::Union(total, w);
    }
  }

  // 頂点が無い・壊れている場合は「十分大きい箱」にしてカリングで消えないようにする
  if (!total.IsValid()) {
    total = RC::BoundingBox::FromCenterHalf({0.0f, 0.0f, 0.0f}, {1.0e4f, 1.0e4f, 1.0e4f});
  }
  localBounds_ = total;
}

void ModelMesh::UploadVB_(const std::vector<VertexData> &vertices) {
  vb_.vertexCount = static_cast<uint32_t>(vertices.size());
  if (vb_.vertexCount == 0)
    return;

  const size_t sizeBytes = sizeof(VertexData) * vb_.vertexCount;

  if (vb_.resource) {
    vb_.resource.Reset();
  }

  vb_.resource = CreateBufferResource(
      device_.Get(), sizeBytes,
      (L"ModelVB: " + std::wstring(sourceInputPath_.begin(), sourceInputPath_.end()))
          .c_str());

  void *mapped = nullptr;
  vb_.resource->Map(0, nullptr, &mapped);
  std::memcpy(mapped, vertices.data(), sizeBytes);
  vb_.resource->Unmap(0, nullptr);

  vb_.view.BufferLocation = vb_.resource->GetGPUVirtualAddress();
  vb_.view.SizeInBytes = static_cast<UINT>(sizeBytes);
  vb_.view.StrideInBytes = sizeof(VertexData);
}

void ModelMesh::UploadIB_() {
  ib_.indexCount = static_cast<uint32_t>(indices_.size());
  if (ib_.indexCount == 0)
    return;

  const size_t sizeBytes = sizeof(uint32_t) * ib_.indexCount;

  if (ib_.resource) {
    ib_.resource.Reset();
  }

  ib_.resource = CreateBufferResource(
      device_.Get(), sizeBytes,
      (L"ModelIB: " + std::wstring(sourceInputPath_.begin(), sourceInputPath_.end()))
          .c_str());

  void *mapped = nullptr;
  ib_.resource->Map(0, nullptr, &mapped);
  std::memcpy(mapped, indices_.data(), sizeBytes);
  ib_.resource->Unmap(0, nullptr);

  ib_.view.BufferLocation = ib_.resource->GetGPUVirtualAddress();
  ib_.view.SizeInBytes = static_cast<UINT>(sizeBytes);
  ib_.view.Format = DXGI_FORMAT_R32_UINT;
}

void ModelMesh::ExtractSkinData_(const aiScene *scene,
                                 std::vector<VertexData> &vertices) {
  if (!scene)
    return;

  // 全メッシュを走査してボーンデータを収集
  for (uint32_t meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
    const aiMesh *mesh = scene->mMeshes[meshIndex];
    if (!mesh || !mesh->HasBones())
      continue;

    // このメッシュの頂点がoutVertices上でどこから始まるか
    const uint32_t vtxOffset = submeshes_[meshIndex].vertexStart;

    for (uint32_t boneIdx = 0; boneIdx < mesh->mNumBones; ++boneIdx) {
      const aiBone *bone = mesh->mBones[boneIdx];
      if (!bone)
        continue;

      const std::string boneName = bone->mName.C_Str();

      // ボーンインデックスを取得または登録
      uint32_t skinBoneIdx;
      auto it = skinData_.jointNameToIndex.find(boneName);
      if (it != skinData_.jointNameToIndex.end()) {
        skinBoneIdx = it->second;
      } else {
        skinBoneIdx = static_cast<uint32_t>(skinData_.inverseBindPoseMatrices.size());
        skinData_.jointNameToIndex[boneName] = skinBoneIdx;

        // InverseBindPoseMatrixを右手系から左手系に変換
        aiMatrix4x4 a = bone->mOffsetMatrix;
        a.Transpose(); // Assimpはcolumn-major、エンジンはrow-major
        RC::Matrix4x4 m = MakeIdentity4x4();
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c)
            m.m[r][c] = a[r][c];
        // aiProcess_MakeLeftHanded により Assimp が座標系変換済みのため手動変換不要
        skinData_.inverseBindPoseMatrices.push_back(m);
      }

      // 各頂点にウェイトを書き込み
      for (uint32_t w = 0; w < bone->mNumWeights; ++w) {
        const aiVertexWeight &vw = bone->mWeights[w];
        const uint32_t globalVtxIdx = vtxOffset + vw.mVertexId;
        if (globalVtxIdx >= vertices.size())
          continue;

        VertexData &v = vertices[globalVtxIdx];

        // 空いているスロットに書き込み（最大4ボーン）
        for (int s = 0; s < 4; ++s) {
          if (v.boneWeights[s] == 0.0f) {
            v.boneIndices[s] = static_cast<int32_t>(skinBoneIdx);
            v.boneWeights[s] = vw.mWeight;
            break;
          }
        }
      }
    }
  }

  // ウェイトの正規化: 合計が1.0になるように調整
  // （4つを超えるボーン影響がある場合や、データの精度問題への対策）
  for (auto &v : vertices) {
    float totalWeight = v.boneWeights[0] + v.boneWeights[1]
                      + v.boneWeights[2] + v.boneWeights[3];
    if (totalWeight > 1e-6f && std::fabs(totalWeight - 1.0f) > 1e-6f) {
      float invTotal = 1.0f / totalWeight;
      for (int s = 0; s < 4; ++s) {
        v.boneWeights[s] *= invTotal;
      }
    }
  }

  if (!skinData_.inverseBindPoseMatrices.empty()) {
    Log::Print(std::format("[ModelMesh] SkinData: {} bones extracted",
                           skinData_.inverseBindPoseMatrices.size()));
  }
}

std::string ModelMesh::ResolveTexturePath_(const aiScene *scene,
                                           const aiMaterial *mat,
                                           const std::string &baseDir) const {
  if (!scene || !mat)
    return {};

  auto TryType = [&](aiTextureType type) -> std::string {
    if (mat->GetTextureCount(type) <= 0)
      return {};

    aiString path;
    if (mat->GetTexture(type, 0, &path) != AI_SUCCESS)
      return {};

    // 埋め込みテクスチャ（\"*0\" とか）
    if (path.length > 0 && path.data[0] == '*') {
      return ExtractEmbeddedTexture_(scene, path, baseDir);
    }

    fs::path p(path.C_Str());
    if (p.is_absolute()) {
      return p.lexically_normal().string();
    }
    return (fs::path(baseDir) / p).lexically_normal().string();
  };

  // glTF2は baseColor が来ることが多い。OBJ系は diffuse。
#if defined(ASSIMP_VERSION_MAJOR) && (ASSIMP_VERSION_MAJOR >= 5)
  if (auto s = TryType(aiTextureType_BASE_COLOR); !s.empty())
    return s;
#endif
  if (auto s = TryType(aiTextureType_DIFFUSE); !s.empty())
    return s;

  // glTF/FBXで \"unknown\" に来ることもある（環境依存）
  if (auto s = TryType(aiTextureType_UNKNOWN); !s.empty())
    return s;

  return {};
}

std::string
ModelMesh::ExtractEmbeddedTexture_(const aiScene *scene,
                                   const aiString &assimpTexPath,
                                   const std::string &baseDir) const {
  if (!scene)
    return {};
  if (assimpTexPath.length < 2)
    return {};
  if (assimpTexPath.data[0] != '*')
    return {};

  const int idx = std::atoi(assimpTexPath.C_Str() + 1);
  if (idx < 0 || idx >= static_cast<int>(scene->mNumTextures))
    return {};

  const aiTexture *tex = scene->mTextures[idx];
  if (!tex)
    return {};

  // 出力先：モデルの横に _assimp_cache フォルダを作る
  fs::path outDir = fs::path(baseDir) / "_assimp_cache";
  std::error_code ec;
  fs::create_directories(outDir, ec);

  std::string ext = "bin";
  if (tex->achFormatHint[0] != '\0') {
    ext = tex->achFormatHint;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  }
  fs::path outPath = outDir / ("embedded_" + std::to_string(idx) + "." + ext);

  // 既に書き出し済みならそれを使う
  if (fs::exists(outPath)) {
    return outPath.string();
  }

  // mHeight == 0 のときは「圧縮済み」(PNG/JPG等) で、mWidth がバイト数
  if (tex->mHeight != 0) {
    // 生のRGBA(aiTexel)のケース。今のTextureManagerがそのまま読めないので一旦スキップ。
    return {};
  }

  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(tex->pcData);
  const size_t size = static_cast<size_t>(tex->mWidth);
  if (!bytes || size == 0)
    return {};

  std::ofstream out(outPath, std::ios::binary);
  if (!out.is_open())
    return {};

  out.write(reinterpret_cast<const char *>(bytes),
            static_cast<std::streamsize>(size));
  out.close();

  return outPath.string();
}

void ModelMesh::CompactMaterials_() {
  if (materials_.empty() || submeshes_.empty()) {
    return;
  }

  // submeshes_ が参照している materialIndex を、登場順でユニーク化
  std::vector<uint32_t> usedOld;
  usedOld.reserve(materials_.size());

  std::vector<int> remap(materials_.size(), -1);

  for (const auto &r : submeshes_) {
    const uint32_t oldIdx = r.materialIndex;
    if (oldIdx >= materials_.size())
      continue;
    if (remap[oldIdx] != -1)
      continue;

    remap[oldIdx] = (int)usedOld.size();
    usedOld.push_back(oldIdx);
  }

  if (usedOld.empty())
    return;

  // すでに "0..N-1" で揃っていれば何もしない
  if (usedOld.size() == materials_.size()) {
    bool identity = true;
    for (uint32_t i = 0; i < usedOld.size(); ++i) {
      if (usedOld[i] != i) {
        identity = false;
        break;
      }
    }
    if (identity)
      return;
  }

  // Material を詰める
  std::vector<MaterialData> newMats;
  newMats.reserve(usedOld.size());
  for (uint32_t oldIdx : usedOld) {
    newMats.push_back(materials_[oldIdx]);
  }
  materials_.swap(newMats);

  // 互換：MaterialFile は「詰めた後の先頭の有効テクスチャ」
  materialFile_ = {};
  for (const auto &md : materials_) {
    if (!md.textureFilePath.empty()) {
      materialFile_ = md;
      break;
    }
  }

  // submeshes_ の index を詰めた後の index に変換
  for (auto &r : submeshes_) {
    const uint32_t oldIdx = r.materialIndex;
    if (oldIdx < remap.size() && remap[oldIdx] >= 0) {
      r.materialIndex = (uint32_t)remap[oldIdx];
    } else {
      r.materialIndex = 0; // fallback
    }
  }
}

RC::Matrix4x4 ModelMesh::MakeAxisFlipX_() {
  RC::Matrix4x4 m = MakeIdentity4x4();
  m.m[0][0] = -1.0f;
  return m;
}

RC::Matrix4x4 ModelMesh::ConvertNodeMatrixRHtoLH_(const RC::Matrix4x4 &m) {
  // row-vector想定：頂点を p' = p * C（CはX反転）で左手化しているので、
  // 行列も M' = C * M * C で同じ座標系に揃える。
  const RC::Matrix4x4 C = MakeAxisFlipX_();
  return Multiply(Multiply(C, m), C);
}
