#include "StartupRegistration.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include <string>

#include <windows.h>

namespace {

const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
const wchar_t kValueName[] = L"PocketBoard";

QString commandLine()
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --background").arg(exe);
}

bool readRunValue(QString *out)
{
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ,
                     nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
        return false;
    std::wstring buf(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ,
                     nullptr, buf.data(), &bytes) != ERROR_SUCCESS)
        return false;
    if (out)
        *out = QString::fromWCharArray(buf.c_str());
    return true;
}

// StartupApproved\Run の値は12バイトのバイナリ。先頭バイトが偶数(0x02等)なら
// 有効、奇数(0x03等)ならユーザーが無効化している。値が無ければ有効扱い。
bool disabledByWindows()
{
    BYTE data[12] = {};
    DWORD bytes = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName, RRF_RT_REG_BINARY,
                     nullptr, data, &bytes) != ERROR_SUCCESS || bytes == 0)
        return false;
    return (data[0] & 0x01) != 0;
}

bool writeRunValue(const QString &value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring w = value.toStdWString();
    const LONG r = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                  reinterpret_cast<const BYTE *>(w.c_str()),
                                  DWORD((w.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool deleteValue(const wchar_t *subKey)
{
    const LONG r = RegDeleteKeyValueW(HKEY_CURRENT_USER, subKey, kValueName);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

} // namespace

namespace StartupRegistration
{

bool isEnabled()
{
    return readRunValue(nullptr) && !disabledByWindows();
}

bool setEnabled(bool enabled)
{
    bool ok = false;
    if (enabled) {
        // 無効フラグを消すと Windows 側も「有効」に戻る。
        ok = writeRunValue(commandLine()) && deleteValue(kApprovedKey);
    } else {
        ok = deleteValue(kRunKey) && deleteValue(kApprovedKey);
    }
    if (!ok)
        qWarning() << "[Startup] failed to" << (enabled ? "register" : "unregister");
    else
        qInfo() << "[Startup]" << (enabled ? "registered:" : "unregistered")
                << (enabled ? commandLine() : QString());
    return ok;
}

void refreshPathIfRegistered()
{
    QString current;
    if (!readRunValue(&current))
        return;
    // 登録先のexeが存在するなら触らない。開発用ビルドを起動しただけで
    // インストール版の自動起動がDebug版に乗っ取られないようにするため。
    // "<path>" --background の形から<path>を取り出す。
    QString registeredExe = current.trimmed();
    if (registeredExe.startsWith(QLatin1Char('"'))) {
        const int end = registeredExe.indexOf(QLatin1Char('"'), 1);
        registeredExe = registeredExe.mid(1, end > 0 ? end - 1 : -1);
    }
    if (QFileInfo::exists(registeredExe))
        return;
    const QString expected = commandLine();
    if (writeRunValue(expected))
        qInfo() << "[Startup] registered exe was missing; updated to:" << expected;
}

} // namespace StartupRegistration
