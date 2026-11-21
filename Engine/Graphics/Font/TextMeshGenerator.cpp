// ============================================================================
// TextMeshGenerator.cpp
// ----------------------------------------------------------------------------
// DirectWrite のグリフアウトライン → Direct2D ジオメトリ演算 → 押し出しメッシュ
//   1. IDWriteFontFace::GetGlyphRunOutline でグリフ 1 文字分のパスを取得
//   2. ID2D1Geometry::Outline  … 重なり合うストロークを 1 つの輪郭に統合（自己交差除去）
//   3. ID2D1Geometry::Tessellate … 表面・裏面用の三角形リスト
//   4. ID2D1Geometry::Simplify(LINES) … 側面用の折れ線輪郭
//   5. 輪郭の外向きを FillContainsPoint で判定し、側面クアッドを生成
// D2D のデバイス／レンダーターゲットは一切作らず、ファクトリ（ジオメトリ演算）のみ使用。
// ============================================================================

#include "TextMeshGenerator.h"
#include "Common/ResourcePath.h"

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Common/Log/Log.h"
#include "Math/Math.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace RC {

namespace {

/// @brief ジオメトリ演算を行う内部座標系での 1em の大きさ
/// @details float 精度と D2D の許容誤差スケールのバランスを取るための定数。
///          最終的に size / kEm でワールド単位へスケールする。
constexpr float kEm = 1024.0f;

/// @brief 縁取りシェルの表裏面を本体から奥へ引っ込める量（em 比）
/// @details 本体の表裏面と同一平面にならない程度に小さく、深度精度で埋もれない程度に大きく。
constexpr float kOutlineInsetEm = 0.003f;

// ----------------------------------------------------------------------------
// 文字列ユーティリティ
// ----------------------------------------------------------------------------

/// @brief UTF-8 文字列を UTF-16 に変換する（フォントパス用）
/// @param s 変換元（UTF-8。不正な場合は ANSI として解釈し直す）
/// @return 変換後のワイド文字列
std::wstring Utf8ToWide_(const std::string &s) {
  if (s.empty()) {
    return {};
  }
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  UINT cp = CP_UTF8;
  if (len <= 0) {
    cp = CP_ACP;
    len = MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  }
  std::wstring w(static_cast<size_t>((std::max)(len, 0)), L'\0');
  if (len > 0) {
    MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
  }
  return w;
}

/// @brief UTF-8 文字列を UTF-32 に変換する
/// @param s 変換元（UTF-8）
/// @return コードポイント列（不正なバイト列は U+FFFD に置き換える）
std::u32string DecodeUtf8_(const std::string &s) {
  std::u32string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    size_t len = 0;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      len = 4;
    } else {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    if (i + len > s.size()) {
      out.push_back(0xFFFD);
      break;
    }
    bool valid = true;
    for (size_t k = 1; k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    out.push_back(valid ? cp : 0xFFFD);
    i += valid ? len : 1;
  }
  return out;
}

// ----------------------------------------------------------------------------
// D2D シンク実装（スタック上で使う軽量 COM オブジェクト）
// ----------------------------------------------------------------------------

/// @brief Simplify(LINES) の出力（折れ線輪郭）を集めるシンク
class PolylineSink final : public ID2D1SimplifiedGeometrySink {
public:
  std::vector<std::vector<D2D1_POINT_2F>> contours; ///< 集めた閉じた折れ線（3 点以上のもののみ）

  // IUnknown（スタック寿命なので参照カウントで delete はしない）
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1SimplifiedGeometrySink)) {
      *ppv = static_cast<ID2D1SimplifiedGeometrySink *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override { return ref_ > 0 ? --ref_ : 0; }

  // ID2D1SimplifiedGeometrySink
  STDMETHODIMP_(void) SetFillMode(D2D1_FILL_MODE) override {}
  STDMETHODIMP_(void) SetSegmentFlags(D2D1_PATH_SEGMENT) override {}
  STDMETHODIMP_(void) BeginFigure(D2D1_POINT_2F start, D2D1_FIGURE_BEGIN) override {
    current_.clear();
    current_.push_back(start);
  }
  STDMETHODIMP_(void) AddLines(const D2D1_POINT_2F *pts, UINT32 n) override {
    current_.insert(current_.end(), pts, pts + n);
  }
  STDMETHODIMP_(void) AddBeziers(const D2D1_BEZIER_SEGMENT *bz, UINT32 n) override {
    // Simplify(LINES) 後は来ないはずだが、保険として粗く平坦化する
    for (UINT32 i = 0; i < n; ++i) {
      const D2D1_POINT_2F p0 = current_.empty() ? bz[i].point1 : current_.back();
      constexpr int kSteps = 8;
      for (int s = 1; s <= kSteps; ++s) {
        const float t = static_cast<float>(s) / kSteps;
        const float u = 1.0f - t;
        D2D1_POINT_2F p;
        p.x = u * u * u * p0.x + 3 * u * u * t * bz[i].point1.x + 3 * u * t * t * bz[i].point2.x + t * t * t * bz[i].point3.x;
        p.y = u * u * u * p0.y + 3 * u * u * t * bz[i].point1.y + 3 * u * t * t * bz[i].point2.y + t * t * t * bz[i].point3.y;
        current_.push_back(p);
      }
    }
  }
  STDMETHODIMP_(void) EndFigure(D2D1_FIGURE_END) override {
    // 終点が始点と一致していれば重複を落とす
    if (current_.size() >= 2) {
      const auto &a = current_.front();
      const auto &b = current_.back();
      if (std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f) {
        current_.pop_back();
      }
    }
    if (current_.size() >= 3) {
      contours.push_back(std::move(current_));
    }
    current_.clear();
  }
  STDMETHODIMP Close() override { return S_OK; }

private:
  ULONG ref_ = 1;                      ///< 参照カウント（寿命管理には使わない）
  std::vector<D2D1_POINT_2F> current_; ///< 組み立て中の図形の点列
};

/// @brief Tessellate の出力（三角形リスト）を集めるシンク
class TriangleSink final : public ID2D1TessellationSink {
public:
  std::vector<D2D1_TRIANGLE> triangles; ///< 集めた三角形

  // IUnknown（スタック寿命なので参照カウントで delete はしない）
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1TessellationSink)) {
      *ppv = static_cast<ID2D1TessellationSink *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override { return ref_ > 0 ? --ref_ : 0; }

  STDMETHODIMP_(void) AddTriangles(const D2D1_TRIANGLE *tris, UINT32 n) override {
    triangles.insert(triangles.end(), tris, tris + n);
  }
  STDMETHODIMP Close() override { return S_OK; }

private:
  ULONG ref_ = 1; ///< 参照カウント（寿命管理には使わない）
};

// ----------------------------------------------------------------------------
// ファクトリ / フォントフェイスのキャッシュ
// ----------------------------------------------------------------------------

/// @struct Factories
/// @brief DirectWrite / Direct2D のファクトリと読み込み済みフォントフェイスのキャッシュ
struct Factories {
  ComPtr<IDWriteFactory> dwrite;        ///< DirectWrite ファクトリ
  ComPtr<ID2D1Factory> d2d;             ///< Direct2D ファクトリ（ジオメトリ演算のみに使う）
  ComPtr<ID2D1StrokeStyle> roundStroke; ///< 縁取りの Widen 用（丸角・丸キャップ）
  std::unordered_map<std::string, ComPtr<IDWriteFontFace>> faces; ///< "パス#フェイス番号" → フォントフェイス
  std::mutex mutex;                     ///< キャッシュとファクトリ操作の排他用
};

/// @brief プロセス全体で共有するファクトリキャッシュを取得する
/// @return 関数内 static のキャッシュへの参照
Factories &Fac_() {
  static Factories f;
  return f;
}

/// @brief DirectWrite / Direct2D のファクトリが未作成なら作成する
/// @param f ファクトリキャッシュ
/// @return 両方のファクトリが使える状態なら true
bool EnsureFactories_(Factories &f) {
  if (!f.dwrite) {
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown **>(f.dwrite.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
      Log::Print("[TextMesh] DWriteCreateFactory に失敗しました");
      return false;
    }
  }
  if (!f.d2d) {
    D2D1_FACTORY_OPTIONS opt{};
    opt.debugLevel = D2D1_DEBUG_LEVEL_NONE;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), &opt,
                                   reinterpret_cast<void **>(f.d2d.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
      Log::Print("[TextMesh] D2D1CreateFactory に失敗しました");
      return false;
    }
  }
  return true;
}

/// @brief フォントフェイスをキャッシュから取得する（無ければ読み込んで登録する）
/// @param f         ファクトリキャッシュ（ファクトリ作成済みであること）
/// @param path      フォントファイルのパス（UTF-8）
/// @param faceIndex フェイスのインデックス（範囲外なら 0 を使う）
/// @return フォントフェイス。読み込めなければ nullptr
ComPtr<IDWriteFontFace> GetFontFace_(Factories &f, const std::string &path, uint32_t faceIndex) {
  const std::string key = path + "#" + std::to_string(faceIndex);
  if (auto it = f.faces.find(key); it != f.faces.end()) {
    return it->second;
  }

  const std::wstring wpath = Utf8ToWide_(Chaso::ResolvePath(path)); // ゲーム側 → エンジン側
  ComPtr<IDWriteFontFile> file;
  HRESULT hr = f.dwrite->CreateFontFileReference(wpath.c_str(), nullptr, &file);
  if (FAILED(hr)) {
    Log::Print("[TextMesh] フォントファイルが開けません: " + Log::NormalizePath(path));
    return nullptr;
  }

  BOOL isSupported = FALSE;
  DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
  DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
  UINT32 numFaces = 0;
  hr = file->Analyze(&isSupported, &fileType, &faceType, &numFaces);
  if (FAILED(hr) || !isSupported || numFaces == 0) {
    Log::Print("[TextMesh] 対応していないフォント形式です: " + Log::NormalizePath(path));
    return nullptr;
  }
  if (faceIndex >= numFaces) {
    Log::Print(std::format("[TextMesh] faceIndex {} は範囲外です（フェイス数 {}）", faceIndex, numFaces));
    faceIndex = 0;
  }

  IDWriteFontFile *files[] = {file.Get()};
  ComPtr<IDWriteFontFace> face;
  hr = f.dwrite->CreateFontFace(faceType, 1, files, faceIndex, DWRITE_FONT_SIMULATIONS_NONE, &face);
  if (FAILED(hr)) {
    Log::Print("[TextMesh] CreateFontFace に失敗しました: " + Log::NormalizePath(path));
    return nullptr;
  }

  f.faces.emplace(key, face);
  Log::Print("[TextMesh] フォント読み込み: " + Log::NormalizePath(path));
  return face;
}

// ----------------------------------------------------------------------------
// グリフ形状（D2D 座標系・1em = kEm・Y 下向き）
// ----------------------------------------------------------------------------

/// @struct GlyphShape
/// @brief 1 グリフ分の表面三角形と側面用輪郭（内部座標系）
struct GlyphShape {
  /// @struct Contour
  /// @brief 側面を作るための閉じた輪郭 1 本
  struct Contour {
    std::vector<D2D1_POINT_2F> pts; ///< 閉じた折れ線（終点＝始点の重複なし）
    float outwardSign = 1.0f;       ///< 辺 (a→b) の外向き法線 = sign * (d.y, -d.x) / |d|
  };
  std::vector<D2D1_TRIANGLE> tris; ///< 表面の三角形
  std::vector<Contour> contours;   ///< 側面用の輪郭
  float advance = 0.0f;            ///< ペン送り量
};

/// @brief 1 グリフのアウトラインを取得して三角形と輪郭に変換する
/// @param f          ファクトリキャッシュ
/// @param face       フォントフェイス
/// @param glyphIndex グリフインデックス
/// @param tolerance  曲線を折れ線化する際の許容誤差（内部単位）
/// @param expand     0 より大きいと、形状を外側へ expand（内部単位）だけ太らせた「縁取り用シェル」を作る
/// @param out        結果の形状
/// @return 成功なら true（空白など形状の無いグリフも true）
bool BuildGlyphShape_(Factories &f, IDWriteFontFace *face, UINT16 glyphIndex, float tolerance,
                      float expand, GlyphShape &out) {
  // 1. アウトライン → パスジオメトリ
  ComPtr<ID2D1PathGeometry> raw;
  if (FAILED(f.d2d->CreatePathGeometry(&raw))) return false;
  {
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(raw->Open(&sink))) return false;
    // TrueType / CFF のグリフは nonzero winding 前提。BeginFigure 前なら複数回呼んでも問題ない
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    HRESULT hr = face->GetGlyphRunOutline(kEm, &glyphIndex, nullptr, nullptr, 1, FALSE, FALSE, sink.Get());
    const HRESULT hrClose = sink->Close();
    if (FAILED(hr) || FAILED(hrClose)) return false;
  }

  UINT32 segCount = 0;
  raw->GetSegmentCount(&segCount);
  if (segCount == 0) {
    return true; // 空白など。三角形なしで成功
  }

  // 1'. 縁取り用シェル：形状を expand だけ外側へ太らせる
  //     Widen（幅 2*expand の丸角ストローク）∪ 元形状 = 元形状を expand だけ膨らませた形
  ComPtr<ID2D1Geometry> src = raw;
  if (expand > 0.0f) {
    if (!f.roundStroke) {
      D2D1_STROKE_STYLE_PROPERTIES props{};
      props.startCap = D2D1_CAP_STYLE_ROUND;
      props.endCap = D2D1_CAP_STYLE_ROUND;
      props.dashCap = D2D1_CAP_STYLE_ROUND;
      props.lineJoin = D2D1_LINE_JOIN_ROUND;
      props.miterLimit = 1.0f;
      props.dashStyle = D2D1_DASH_STYLE_SOLID;
      props.dashOffset = 0.0f;
      if (FAILED(f.d2d->CreateStrokeStyle(&props, nullptr, 0, &f.roundStroke))) return false;
    }
    ComPtr<ID2D1PathGeometry> widened;
    if (FAILED(f.d2d->CreatePathGeometry(&widened))) return false;
    {
      ComPtr<ID2D1GeometrySink> sink;
      if (FAILED(widened->Open(&sink))) return false;
      HRESULT hr = raw->Widen(expand * 2.0f, f.roundStroke.Get(),
                              static_cast<const D2D1_MATRIX_3X2_F *>(nullptr), tolerance, sink.Get());
      const HRESULT hrClose = sink->Close();
      if (FAILED(hr) || FAILED(hrClose)) return false;
    }
    ComPtr<ID2D1PathGeometry> expanded;
    if (FAILED(f.d2d->CreatePathGeometry(&expanded))) return false;
    {
      ComPtr<ID2D1GeometrySink> sink;
      if (FAILED(expanded->Open(&sink))) return false;
      HRESULT hr = raw->CombineWithGeometry(widened.Get(), D2D1_COMBINE_MODE_UNION,
                                            static_cast<const D2D1_MATRIX_3X2_F *>(nullptr), tolerance, sink.Get());
      const HRESULT hrClose = sink->Close();
      if (FAILED(hr) || FAILED(hrClose)) return false;
    }
    src = expanded;
  }

  // 2. 自己交差・重なりの除去（日本語フォントの重なるストローク対策）
  ComPtr<ID2D1Geometry> clean;
  {
    ComPtr<ID2D1PathGeometry> outlined;
    if (FAILED(f.d2d->CreatePathGeometry(&outlined))) return false;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(outlined->Open(&sink))) return false;
    HRESULT hr = src->Outline(static_cast<const D2D1_MATRIX_3X2_F *>(nullptr), tolerance, sink.Get());
    const HRESULT hrClose = sink->Close();
    // Outline が失敗した場合は元のジオメトリで続行
    clean = (FAILED(hr) || FAILED(hrClose)) ? src : ComPtr<ID2D1Geometry>(outlined);
  }

  // 3. 曲線を折れ線化した「直線だけ」のジオメトリを 1 つ作る。
  //    表面の三角形分割と側面の輪郭を同じ折れ線から作ることで、
  //    表面の縁と側面の頂点が一致し、隙間や T 字接合が生じない。
  ComPtr<ID2D1PathGeometry> poly;
  if (FAILED(f.d2d->CreatePathGeometry(&poly))) return false;
  {
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(poly->Open(&sink))) return false;
    HRESULT hr = clean->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES,
                                 static_cast<const D2D1_MATRIX_3X2_F *>(nullptr), tolerance, sink.Get());
    const HRESULT hrClose = sink->Close();
    if (FAILED(hr) || FAILED(hrClose)) return false;
  }

  // 4. 表面の三角形（poly は直線のみなので、ここでの許容誤差は実質無関係）
  {
    TriangleSink tsink;
    if (FAILED(poly->Tessellate(static_cast<const D2D1_MATRIX_3X2_F *>(nullptr), tolerance, &tsink))) {
      return false;
    }
    out.tris = std::move(tsink.triangles);
  }

  // 5. 側面用の折れ線輪郭（poly をそのまま吐き出す）
  {
    PolylineSink psink;
    if (FAILED(poly->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES,
                              static_cast<const D2D1_MATRIX_3X2_F *>(nullptr), tolerance, &psink))) {
      return false;
    }
    out.contours.reserve(psink.contours.size());
    for (auto &c : psink.contours) {
      GlyphShape::Contour contour;
      contour.pts = std::move(c);
      out.contours.push_back(std::move(contour));
    }
  }

  // 6. 各輪郭の外向き法線の符号を決める
  //    Outline 後のジオメトリは自己交差が無く向きも一貫している（塗り領域が常に進行方向の
  //    同じ側にある）ので、基本は「最大面積の輪郭（必ず外周）の回転方向」から全輪郭共通の
  //    符号を求める。さらに輪郭ごとに、法線方向へずらした点の内外判定が決定的なら
  //    そちらを優先する（向きの一貫性に依存しない保険）。
  auto signedArea2 = [](const std::vector<D2D1_POINT_2F> &pts) {
    float a = 0.0f;
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i) {
      const auto &p = pts[i];
      const auto &q = pts[(i + 1) % n];
      a += p.x * q.y - q.x * p.y;
    }
    return a; // 2 倍の符号付き面積
  };
  float globalSign = 1.0f;
  {
    float best = 0.0f;
    for (const auto &c : out.contours) {
      const float a = signedArea2(c.pts);
      if (std::fabs(a) > std::fabs(best)) best = a;
    }
    // 符号付き面積が正なら辺 (a→b) の外向きは (dy, -dx)、負ならその逆
    if (best < 0.0f) globalSign = -1.0f;
  }

  // 内外判定は FillContainsPoint の許容誤差を小さくし、プローブ距離をそれより十分大きく取る
  // （許容誤差以内の点は「内側」と判定されてしまうため）
  const float probeTol = tolerance * 0.05f;
  const float eps = tolerance * 2.0f;
  for (auto &c : out.contours) {
    c.outwardSign = globalSign;

    const size_t n = c.pts.size();
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    auto edgeLen2 = [&](size_t i) {
      const auto &a = c.pts[i];
      const auto &b = c.pts[(i + 1) % n];
      const float dx = b.x - a.x, dy = b.y - a.y;
      return dx * dx + dy * dy;
    };
    const size_t probeCount = (std::min<size_t>)(n, 3);
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(probeCount), order.end(),
                      [&](size_t l, size_t r) { return edgeLen2(l) > edgeLen2(r); });

    for (size_t k = 0; k < probeCount; ++k) {
      const size_t i = order[k];
      const auto &a = c.pts[i];
      const auto &b = c.pts[(i + 1) % n];
      const float dx = b.x - a.x, dy = b.y - a.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len < eps) continue; // 短すぎる辺では判定が不安定
      const float nx = dy / len, ny = -dx / len;
      const D2D1_POINT_2F mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
      const D2D1_POINT_2F pPlus{mid.x + nx * eps, mid.y + ny * eps};
      const D2D1_POINT_2F pMinus{mid.x - nx * eps, mid.y - ny * eps};
      BOOL inPlus = FALSE, inMinus = FALSE;
      const HRESULT h1 = poly->FillContainsPoint(pPlus, static_cast<const D2D1_MATRIX_3X2_F *>(nullptr), probeTol, &inPlus);
      const HRESULT h2 = poly->FillContainsPoint(pMinus, static_cast<const D2D1_MATRIX_3X2_F *>(nullptr), probeTol, &inMinus);
      if (SUCCEEDED(h1) && SUCCEEDED(h2) && inPlus != inMinus) {
        c.outwardSign = inPlus ? -1.0f : 1.0f;
        break;
      }
    }
  }

  return true;
}

// ----------------------------------------------------------------------------
// メッシュ組み立て
// ----------------------------------------------------------------------------

/// @struct MeshBuilder
/// @brief ModelData へ頂点と三角形を追加する補助
struct MeshBuilder {
  ModelData &out; ///< 書き込み先

  /// @brief 三角形を追加する（面法線が wantNormal と逆向きなら巻き順を反転する）
  /// @param i0         頂点インデックス 0
  /// @param i1         頂点インデックス 1
  /// @param i2         頂点インデックス 2
  /// @param wantNormal 表にしたい向き
  /// @note 面積 0 の退化三角形は追加しない。
  void PushTriangle(uint32_t i0, uint32_t i1, uint32_t i2, const Vector3 &wantNormal) {
    const auto &a = out.vertices[i0].position;
    const auto &b = out.vertices[i1].position;
    const auto &c = out.vertices[i2].position;
    const float e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
    const float e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
    const float cx = e1y * e2z - e1z * e2y;
    const float cy = e1z * e2x - e1x * e2z;
    const float cz = e1x * e2y - e1y * e2x;
    const float len2 = cx * cx + cy * cy + cz * cz;
    if (!(len2 > 0.0f)) return; // 退化三角形（size が極小でも落とさないよう絶対しきい値は使わない）
    const float d = cx * wantNormal.x + cy * wantNormal.y + cz * wantNormal.z;
    if (d < 0.0f) std::swap(i1, i2);
    out.indices.push_back(i0);
    out.indices.push_back(i1);
    out.indices.push_back(i2);
  }

  /// @brief 頂点を追加する
  /// @param x  位置 X
  /// @param y  位置 Y
  /// @param z  位置 Z
  /// @param n  法線
  /// @param uv テクスチャ座標
  /// @return 追加した頂点のインデックス
  uint32_t PushVertex(float x, float y, float z, const Vector3 &n, const Vector2 &uv = {0.0f, 0.0f}) {
    VertexData v{};
    v.position = {x, y, z, 1.0f};
    v.normal = n;
    v.texcoord = uv;
    out.vertices.push_back(v);
    return static_cast<uint32_t>(out.vertices.size() - 1);
  }
};

/// @brief 文字メッシュ生成の実体
/// @param desc     生成パラメータ
/// @param expandEm 形状を外側へ太らせる量（em 比、0 で本体）
/// @param zFront   表面（法線 -Z）の Z 座標
/// @param zBack    裏面（法線 +Z）の Z 座標。zFront 以下なら板ポリ（表面のみ）
/// @param out      出力先モデルデータ（クリアしてから書き込む）
/// @param outInfo  AABB・頂点数（不要なら nullptr）
/// @return 1 つ以上の三角形が生成できたら true
bool GenerateImpl_(const TextMeshDesc &desc, float expandEm, float zFront, float zBack,
                   ModelData &out, TextMeshInfo *outInfo) {
  out.vertices.clear();
  out.indices.clear();
  out.material = {};
  out.rootNode = {};
  out.rootNode.name = expandEm > 0.0f ? "TextMeshOutline" : "TextMesh";
  out.rootNode.localMatrix = MakeIdentity4x4();

  if (desc.text.empty() || desc.fontPath.empty() || desc.size <= 0.0f) {
    return false;
  }

  Factories &f = Fac_();
  std::lock_guard<std::mutex> lock(f.mutex);
  if (!EnsureFactories_(f)) return false;

  ComPtr<IDWriteFontFace> face = GetFontFace_(f, desc.fontPath, desc.faceIndex);
  if (!face) return false;

  // --- フォントメトリクス（デザイン単位 → 内部単位） ---
  DWRITE_FONT_METRICS fm{};
  face->GetMetrics(&fm);
  if (fm.designUnitsPerEm == 0) return false;
  const float designScale = kEm / static_cast<float>(fm.designUnitsPerEm);
  const float lineHeight =
      static_cast<float>(fm.ascent + fm.descent + fm.lineGap) * designScale * (std::max)(desc.lineSpacing, 0.0f);

  const float tolerance = (std::max)(desc.curveTolerance, 0.0005f) * kEm;
  const float scale = desc.size / kEm;
  const float expand = (std::max)(expandEm, 0.0f) * kEm;
  const bool solid = zBack > zFront; // false なら表面だけの板ポリ

  // --- 行分割 & グリフインデックス取得 ---
  struct Line {
    std::vector<UINT16> glyphs;  ///< グリフインデックス列
    std::vector<float> advances; ///< 各グリフの送り量（内部単位）
    float width = 0.0f;          ///< 行全体の幅（内部単位）
  };
  std::vector<Line> lines;
  {
    const std::u32string cps = DecodeUtf8_(desc.text);
    std::vector<UINT32> lineCps;
    auto flush = [&]() {
      Line line;
      if (!lineCps.empty()) {
        // タブは空白グリフで引き、送り量だけ 4 倍にする（.notdef の豆腐を出さない）
        std::vector<bool> isTab(lineCps.size(), false);
        for (size_t i = 0; i < lineCps.size(); ++i) {
          if (lineCps[i] == U'\t') {
            isTab[i] = true;
            lineCps[i] = U' ';
          }
        }
        line.glyphs.resize(lineCps.size());
        if (FAILED(face->GetGlyphIndices(lineCps.data(), static_cast<UINT32>(lineCps.size()), line.glyphs.data()))) {
          std::fill(line.glyphs.begin(), line.glyphs.end(), static_cast<UINT16>(0));
        }
        std::vector<DWRITE_GLYPH_METRICS> gm(lineCps.size());
        if (SUCCEEDED(face->GetDesignGlyphMetrics(line.glyphs.data(), static_cast<UINT32>(line.glyphs.size()), gm.data(), FALSE))) {
          line.advances.resize(gm.size());
          for (size_t i = 0; i < gm.size(); ++i) {
            line.advances[i] = static_cast<float>(gm[i].advanceWidth) * designScale;
            if (isTab[i]) line.advances[i] *= 4.0f;
            line.width += line.advances[i];
          }
        } else {
          line.advances.assign(line.glyphs.size(), kEm * 0.5f);
          line.width = kEm * 0.5f * static_cast<float>(line.glyphs.size());
        }
      }
      lines.push_back(std::move(line));
      lineCps.clear();
    };
    for (char32_t c : cps) {
      if (c == U'\n') {
        flush();
      } else if (c == U'\r') {
        continue;
      } else {
        lineCps.push_back(static_cast<UINT32>(c));
      }
    }
    flush();
  }

  // --- グリフ形状のキャッシュ（同じ文字は一度だけ演算） ---
  std::unordered_map<UINT16, GlyphShape> shapes;
  auto getShape = [&](UINT16 gi) -> const GlyphShape * {
    if (auto it = shapes.find(gi); it != shapes.end()) return &it->second;
    GlyphShape shape;
    if (!BuildGlyphShape_(f, face.Get(), gi, tolerance, expand, shape)) {
      shape = {};
    }
    auto inserted = shapes.emplace(gi, std::move(shape));
    return &inserted.first->second;
  };

  // --- メッシュ組み立て ---
  MeshBuilder mb{out};
  const Vector3 nFront{0.0f, 0.0f, -1.0f};
  const Vector3 nBack{0.0f, 0.0f, 1.0f};

  // 内部座標 (D2D, Y 下向き) → ローカル座標 (Y 上向き) 変換
  auto toWorld = [&](float x, float y, float penX, float penY) -> Vector2 {
    return {(x + penX) * scale, -(y + penY) * scale};
  };

  for (size_t li = 0; li < lines.size(); ++li) {
    const Line &line = lines[li];
    float penX = 0.0f;
    if (desc.align == TextAlign::Center) penX = -line.width * 0.5f;
    else if (desc.align == TextAlign::Right) penX = -line.width;
    const float penY = static_cast<float>(li) * lineHeight;

    for (size_t gi = 0; gi < line.glyphs.size(); ++gi) {
      const GlyphShape *shape = getShape(line.glyphs[gi]);
      const float gx = penX;
      penX += (gi < line.advances.size()) ? line.advances[gi] : 0.0f;
      if (!shape || shape->tris.empty()) continue;

      // 表面 / 裏面（頂点を共有するため座標 → インデックスの辞書を作る）
      struct Key {
        float x, y;
        bool operator==(const Key &o) const { return x == o.x && y == o.y; }
      };
      struct KeyHash {
        size_t operator()(const Key &k) const {
          // -0.0f と +0.0f は == で等しいのでハッシュも揃える（+0.0f を足すと -0.0f → +0.0f）
          const float x = k.x + 0.0f, y = k.y + 0.0f;
          uint32_t a, b;
          std::memcpy(&a, &x, 4);
          std::memcpy(&b, &y, 4);
          return std::hash<uint64_t>{}((static_cast<uint64_t>(a) << 32) | b);
        }
      };
      std::unordered_map<Key, uint32_t, KeyHash> frontMap, backMap;
      frontMap.reserve(shape->tris.size() * 2);
      backMap.reserve(shape->tris.size() * 2);

      auto frontIdx = [&](const D2D1_POINT_2F &p) {
        const Key k{p.x, p.y};
        if (auto it = frontMap.find(k); it != frontMap.end()) return it->second;
        const Vector2 w = toWorld(p.x, p.y, gx, penY);
        const uint32_t idx = mb.PushVertex(w.x, w.y, zFront, nFront);
        frontMap.emplace(k, idx);
        return idx;
      };
      auto backIdx = [&](const D2D1_POINT_2F &p) {
        const Key k{p.x, p.y};
        if (auto it = backMap.find(k); it != backMap.end()) return it->second;
        const Vector2 w = toWorld(p.x, p.y, gx, penY);
        const uint32_t idx = mb.PushVertex(w.x, w.y, zBack, nBack);
        backMap.emplace(k, idx);
        return idx;
      };

      for (const auto &t : shape->tris) {
        mb.PushTriangle(frontIdx(t.point1), frontIdx(t.point2), frontIdx(t.point3), nFront);
      }
      if (solid) {
        for (const auto &t : shape->tris) {
          mb.PushTriangle(backIdx(t.point1), backIdx(t.point2), backIdx(t.point3), nBack);
        }

        // 側面（辺ごとにフラットシェーディング用の独立頂点）
        // UV: u = 輪郭に沿った周長（1em = 1.0 で繰り返し）、v = 厚さ方向 0〜1（表面側が 0）
        for (const auto &c : shape->contours) {
          const size_t n = c.pts.size();
          float runLen = 0.0f;
          for (size_t i = 0; i < n; ++i) {
            const auto &a = c.pts[i];
            const auto &b = c.pts[(i + 1) % n];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (!(len > 0.0f)) continue;
            // D2D 座標での外向き法線 → Y 反転してローカル座標へ
            const float nx = c.outwardSign * (dy / len);
            const float ny = c.outwardSign * (-dx / len);
            const Vector3 nSide{nx, -ny, 0.0f};

            const float u0 = runLen / kEm;
            const float u1 = (runLen + len) / kEm;
            runLen += len;

            const Vector2 wa = toWorld(a.x, a.y, gx, penY);
            const Vector2 wb = toWorld(b.x, b.y, gx, penY);
            const uint32_t aF = mb.PushVertex(wa.x, wa.y, zFront, nSide, {u0, 0.0f});
            const uint32_t bF = mb.PushVertex(wb.x, wb.y, zFront, nSide, {u1, 0.0f});
            const uint32_t bB = mb.PushVertex(wb.x, wb.y, zBack, nSide, {u1, 1.0f});
            const uint32_t aB = mb.PushVertex(wa.x, wa.y, zBack, nSide, {u0, 1.0f});
            mb.PushTriangle(aF, bF, bB, nSide);
            mb.PushTriangle(aF, bB, aB, nSide);
          }
        }
      }
    }
  }

  if (out.vertices.empty() || out.indices.empty()) {
    out.vertices.clear();
    out.indices.clear();
    return false;
  }

  // --- バウンディングボックス & 表裏面の UV（文字列全体で 0〜1 の平面投影、上が v=0） ---
  Vector3 mn{FLT_MAX, FLT_MAX, FLT_MAX};
  Vector3 mx{-FLT_MAX, -FLT_MAX, -FLT_MAX};
  for (const auto &v : out.vertices) {
    mn.x = (std::min)(mn.x, v.position.x);
    mn.y = (std::min)(mn.y, v.position.y);
    mn.z = (std::min)(mn.z, v.position.z);
    mx.x = (std::max)(mx.x, v.position.x);
    mx.y = (std::max)(mx.y, v.position.y);
    mx.z = (std::max)(mx.z, v.position.z);
  }
  const float w = (std::max)(mx.x - mn.x, 1e-6f);
  const float h = (std::max)(mx.y - mn.y, 1e-6f);
  for (auto &v : out.vertices) {
    if (std::fabs(v.normal.z) > 0.5f) {
      v.texcoord = {(v.position.x - mn.x) / w, (mx.y - v.position.y) / h};
    }
    // 側面の UV は生成時に周長ベースで設定済み
  }

  if (outInfo) {
    outInfo->min = mn;
    outInfo->max = mx;
    outInfo->vertexCount = static_cast<uint32_t>(out.vertices.size());
    outInfo->triangleCount = static_cast<uint32_t>(out.indices.size() / 3);
  }
  return true;
}

} // namespace

// ============================================================================
// 公開関数
// ============================================================================

bool TextMeshGenerator::Generate(const TextMeshDesc &desc, ModelData &out, TextMeshInfo *outInfo) {
  const float halfDepth = (std::max)(desc.depth, 0.0f) * 0.5f;
  return GenerateImpl_(desc, 0.0f, -halfDepth, +halfDepth, out, outInfo);
}

bool TextMeshGenerator::GenerateOutline(const TextMeshDesc &desc, float outlineWidth, ModelData &out,
                                        TextMeshInfo *outInfo) {
  if (outlineWidth <= 0.0f) {
    out.vertices.clear();
    out.indices.clear();
    return false;
  }
  // 本体より僅かに奥まった位置に表裏面を置く：
  //   - 表面同士が同一平面にならないので Z ファイトしない
  //   - 本体の表面が常に手前で勝つため、文字の周囲に幅 outlineWidth の縁だけが見える
  //   - 側面は本体より外側なので、横から見ても縁取り色になる
  // 薄い文字でもシェルが側面を持てるよう、引っ込め量は厚さの 1/4 を上限にする。
  // 厚さ 0（板ポリ）のときはシェルも板ポリになり、本体の僅かに後ろに置かれる。
  // NOTE: 引っ込め量は固定（em 比）なので、極端に遠い/小さい文字では深度精度が足りず
  //       本体と縁の境目がちらつく可能性がある。その場合は size か depth を大きくする。
  const float halfDepth = (std::max)(desc.depth, 0.0f) * 0.5f;
  float inset = desc.size * kOutlineInsetEm;
  if (halfDepth > 0.0f) inset = (std::min)(inset, halfDepth * 0.25f);
  return GenerateImpl_(desc, outlineWidth, -halfDepth + inset, +halfDepth - inset, out, outInfo);
}

void TextMeshGenerator::ClearCache() {
  Factories &f = Fac_();
  std::lock_guard<std::mutex> lock(f.mutex);
  f.faces.clear();
  f.roundStroke.Reset();
  f.d2d.Reset();
  f.dwrite.Reset();
}

} // namespace RC
