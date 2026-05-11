#include "ui/MainWindow.h"

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QString>

#include <cstdio>

namespace {

/// Pre-Qt argv scan for `--data-dir <path>` or `--data-dir=<path>`.
/// Returns the path if present, or an empty QString. Doesn't remove
/// the flag from argv — Qt ignores unknown options.
QString parseDataDirFlag(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--data-dir") && i + 1 < argc) {
            return QString::fromLocal8Bit(argv[i + 1]);
        }
        if (a.startsWith(QStringLiteral("--data-dir="))) {
            return a.mid(QStringLiteral("--data-dir=").size());
        }
    }
    return {};
}

} // namespace

int main(int argc, char* argv[])
{
    // Honour `--data-dir <path>` BEFORE QApplication is constructed so
    // every QStandardPaths::HomeLocation lookup in the rest of the
    // codebase resolves under the override. On Windows, Qt's
    // HomeLocation reads %USERPROFILE%; on Linux/Mac it reads $HOME.
    // Setting both keeps the override portable.
    const QString dataDir = parseDataDirFlag(argc, argv);
    if (!dataDir.isEmpty()) {
        const QString absolute = QFileInfo(dataDir).absoluteFilePath();
        QDir().mkpath(absolute);
#if defined(Q_OS_WIN)
        qputenv("USERPROFILE", absolute.toLocal8Bit());
#else
        qputenv("HOME", absolute.toLocal8Bit());
#endif
        std::fprintf(stderr,
                     "censorcut: --data-dir override active, "
                     "user data under %s\n",
                     absolute.toLocal8Bit().constData());
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("CensorCut"));
    QApplication::setOrganizationName(QStringLiteral("CensorCut"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    censorcut::MainWindow w;
    w.show();
    return app.exec();
}
