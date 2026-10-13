#pragma once

#include <QHash>
#include <QOpenGLShaderProgram>

// ---- UniformLocationCache（2026-09-16）---------------------------------------
// QOpenGLShaderProgram::setUniformValue("名前", 値) は、呼ぶたびに内部で
// glGetUniformLocation（文字列から引数の番号を探す）を実行する。
// ガラスのハイライトは1回描くたびにこれを約30回、グローや図形も数回ずつ
// 行っていて、パンやズームで全バブルを描き直すフレームではバブル数に比例して
// 積み上がっていた。
//
// 引数の番号はシェーダーをリンクした時点で決まり、その後は変わらないので、
// 最初の1回だけ探して覚えておく。毎フレーム変わる「値」（ライトの向きや
// 位置など）は今までどおり毎回設定するので、見た目・挙動は一切変わらない。
//
// キーは名前の文字列リテラルのポインタ（同じ呼び出し箇所なら常に同じ）。
// 同じ名前が別のポインタで来ても、エントリが1つ増えるだけで結果は正しい。
// シェーダーを作り直したら clear() すること（番号が変わりうるため）。
class UniformLocationCache
{
public:
    int get(QOpenGLShaderProgram *program, const char *name)
    {
        QHash<const char *, int> &table = m_tables[program];
        const auto it = table.constFind(name);
        if (it != table.constEnd())
            return it.value();
        const int location = program->uniformLocation(name);
        table.insert(name, location);
        return location;
    }

    void clear() { m_tables.clear(); }

private:
    QHash<QOpenGLShaderProgram *, QHash<const char *, int>> m_tables;
};
