#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>
#include "ScriptableEntity.h"

/// @class ScriptRegistry
/// @brief スクリプトの型名と生成関数を対応付けるレジストリ
/// @details エディタ UI などから、C++ スクリプトを名前だけで動的に生成できるようにする。
class ScriptRegistry {
public:
    using InstantiateFn = std::function<ScriptableEntity*()>; ///< スクリプトを new して返す生成関数

    /// @brief スクリプトの型を名前で登録する
    /// @param name 登録名（通常は型名）
    /// @param func 生成関数
    /// @note 同じ名前が既に登録済みなら何もしない（先に登録したものが残る）。
    static void Register(const std::string& name, InstantiateFn func);

    /// @brief 名前からスクリプトを生成する
    /// @param name 登録名
    /// @return 生成したスクリプト（未登録の名前なら nullptr）。所有権は呼び出し側に移る
    static ScriptableEntity* Instantiate(const std::string& name);

    /// @brief 登録済みのスクリプト名一覧を取得する
    /// @return 登録順に並んだスクリプト名の配列
    static const std::vector<std::string>& GetScriptNames();

private:
    /// @brief 名前 → 生成関数の表を取得する
    /// @return 関数内 static の表（静的初期化順の問題を避けるため関数内に置く）
    static std::unordered_map<std::string, InstantiateFn>& GetRegistry();

    /// @brief 登録順の名前一覧を取得する
    /// @return 関数内 static の名前配列
    static std::vector<std::string>& GetNames();
};

/// @brief ScriptableEntity の派生クラスを main() より前に自動登録するマクロ
/// @details スクリプトの .cpp 末尾に書く（.h のグローバルスコープでも動くが、.cpp の方が安全）。
#define REGISTER_SCRIPT(Type) \
    namespace { \
        struct ScriptRegisterer_##Type { \
            ScriptRegisterer_##Type() { \
                ScriptRegistry::Register(#Type, []() { return new Type(); }); \
            } \
        }; \
        static ScriptRegisterer_##Type g_ScriptRegisterer_##Type; \
    }
