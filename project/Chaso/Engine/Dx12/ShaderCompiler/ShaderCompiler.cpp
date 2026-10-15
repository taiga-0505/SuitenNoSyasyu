#include "ShaderCompiler.h"
#include "Common/ResourcePath.h"
#include <cassert>
#include <filesystem>
#include <format>
#include <Windows.h>

using Microsoft::WRL::ComPtr;

namespace {
/// @brief wstring を UTF-8 の string に変換する（ログ出力用）
std::string ToUtf8(const std::wstring &w) {
  if (w.empty())
    return {};
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                        nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len,
                        nullptr, nullptr);
  return s;
}
} // namespace

ShaderCompiler::ShaderCompiler() {}
ShaderCompiler::~ShaderCompiler() { Term(); }

bool ShaderCompiler::Init() {
  HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils_));
  if (FAILED(hr))
    return false;
  hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_));
  if (FAILED(hr))
    return false;
  hr = utils_->CreateDefaultIncludeHandler(&includeHandler_);
  return SUCCEEDED(hr);
}

void ShaderCompiler::Term() {
  includeHandler_.Reset();
  compiler_.Reset();
  utils_.Reset();
}

CompiledShader ShaderCompiler::Compile(const ShaderDesc &desc) const {
  // NOTE: assert は Release で消えるうえ Debug では下のメッセージに届かないので使わない
  if (!utils_ || !compiler_ || !includeHandler_) {
    return CompiledShader(
        "ShaderCompiler が初期化されていません (ShaderCompiler::Init 失敗)");
  }

  // ファイル読み込み
  // ゲーム側 Resources → エンジン側（Chaso/Resources）の順で探す
  const std::wstring srcPath = Chaso::ResolvePath(desc.path);
  ComPtr<IDxcBlobEncoding> srcBlob;
  HRESULT hr = utils_->LoadFile(srcPath.c_str(), nullptr, &srcBlob);
  if (FAILED(hr)) {
    // 大半は「カレントディレクトリ違い」や「パスの解決失敗」でファイルに辿り着けていないケース。
    // パスのままだと原因が分からないので絶対パスまで含めて報告する。
    std::string absPath;
    std::error_code ec;
    const auto abs = std::filesystem::absolute(srcPath, ec);
    absPath = ec ? "(絶対パス解決失敗)" : ToUtf8(abs.wstring());

    const auto cwd = std::filesystem::current_path(ec);
    const std::string cwdStr = ec ? "(取得失敗)" : ToUtf8(cwd.wstring());

    return CompiledShader(std::format(
        "シェーダーファイルを開けません (hr=0x{:08X})\n"
        "  指定パス   : {}\n"
        "  解決パス   : {}\n"
        "  絶対パス   : {}\n"
        "  作業ディレクトリ: {}\n"
        "  → パス指定や作業ディレクトリが正しいか確認してください。",
        static_cast<unsigned>(hr), ToUtf8(desc.path), ToUtf8(srcPath), absPath, cwdStr));
  }

  // 引数組み立て
  std::vector<LPCWSTR> args;
  args.push_back(srcPath.c_str()); // ソース名（#include の基準ディレクトリにもなる）
  args.push_back(L"-E");
  args.push_back(desc.entry.c_str());
  args.push_back(L"-T");
  args.push_back(desc.target.c_str());
  args.push_back(L"-Zpr");
  if (desc.optimize) {
    args.push_back(L"-O3");
  } else {
    args.push_back(L"-Od");
  }
  if (desc.debugInfo) {
    args.push_back(L"-Zi");
    args.push_back(L"-Qembed_debug");
  }
  // リフレクション情報をバイナリから取り除く（任意）
  args.push_back(L"-Qstrip_reflect"); // バイナリを最小化したい場合

  // マクロ定義
  std::vector<DxcDefine> defs = desc.defines; // コピー（必要なら外で保持）。現状は args に渡していない
  DxcBuffer src{};
  src.Ptr = srcBlob->GetBufferPointer();
  src.Size = srcBlob->GetBufferSize();
  src.Encoding = DXC_CP_UTF8;

  ComPtr<IDxcResult> result;
  hr = compiler_->Compile(&src, args.data(), (UINT)args.size(),
                          includeHandler_.Get(), IID_PPV_ARGS(&result));
  if (FAILED(hr) || !result) {
    return CompiledShader(
        std::format("IDxcCompiler3::Compile 呼び出しに失敗 (hr=0x{:08X}): {}",
                    static_cast<unsigned>(hr), ToUtf8(desc.path)));
  }

  ComPtr<IDxcBlobUtf8> errors;
  result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

  HRESULT status = S_OK;
  result->GetStatus(&status);
  if (FAILED(status)) {
    // 失敗。ログを返す
    return CompiledShader(nullptr, errors);
  }

  ComPtr<IDxcBlob> obj;
  result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr);
  return CompiledShader(obj, errors);
}
