#include "AnalysisController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace censorcut {

namespace {

constexpr int kStderrTailMax = 64 * 1024;

void appendBounded(QByteArray& tail, const QByteArray& chunk)
{
    tail.append(chunk);
    if (tail.size() > kStderrTailMax)
        tail.remove(0, tail.size() - kStderrTailMax);
}

} // namespace

AnalysisController::AnalysisController(QObject* parent)
    : QObject(parent)
{
    connect(&m_proc, &QProcess::readyReadStandardOutput,
            this, &AnalysisController::onStdoutReady);
    connect(&m_proc, &QProcess::readyReadStandardError,
            this, &AnalysisController::onStderrReady);
    connect(&m_proc, &QProcess::finished,
            this, &AnalysisController::onProcessFinished);
    connect(&m_proc, &QProcess::errorOccurred,
            this, &AnalysisController::onErrorOccurred);
}

AnalysisController::~AnalysisController()
{
    if (m_proc.state() != QProcess::NotRunning) {
        m_proc.kill();
        m_proc.waitForFinished(2000);
    }
}

QString AnalysisController::locatePython()
{
#if defined(Q_OS_WIN)
    // Windows: prefer 'python' (Microsoft Store / installer) then 'py.exe'
    // (the Windows Python launcher).
    for (const QString& exe : { QStringLiteral("python"), QStringLiteral("py") }) {
        const QString p = QStandardPaths::findExecutable(exe);
        if (!p.isEmpty()) return p;
    }
#else
    for (const QString& exe : { QStringLiteral("python3"), QStringLiteral("python") }) {
        const QString p = QStandardPaths::findExecutable(exe);
        if (!p.isEmpty()) return p;
    }
#endif
    return {};
}

QString AnalysisController::locatePythonPackageDir()
{
    // Look for python/censorcut/__init__.py walking up from the app dir.
    QDir d(QCoreApplication::applicationDirPath());
    for (int hops = 0; hops < 6; ++hops) {
        const QString candidate = d.filePath(QStringLiteral("python/censorcut/__init__.py"));
        if (QFileInfo::exists(candidate))
            return d.filePath(QStringLiteral("python"));
        if (!d.cdUp()) break;
    }
    return {};
}

void AnalysisController::setPythonPath(const QString& path)
{
    m_pythonPath = path;
}

bool AnalysisController::isRunning() const
{
    return m_proc.state() != QProcess::NotRunning;
}

bool AnalysisController::start(const QString& inputPath)
{
    if (isRunning()) return false;

    if (m_pythonPath.isEmpty()) m_pythonPath = locatePython();
    if (m_pythonPath.isEmpty()) {
        emit failed(QStringLiteral("Python interpreter not found. Install Python 3.9+ and "
                                   "make sure 'python' is on PATH."));
        return false;
    }
    if (m_packageDir.isEmpty()) m_packageDir = locatePythonPackageDir();
    if (m_packageDir.isEmpty()) {
        emit failed(QStringLiteral("Could not find the censorcut analyzer package. "
                                   "Expected python/censorcut/ next to the build."));
        return false;
    }
    if (!QFileInfo(inputPath).isFile()) {
        emit failed(QStringLiteral("Input video not found: %1").arg(inputPath));
        return false;
    }

    // Output JSON goes into a temp file we own.
    const QString uniq = QStringLiteral("censorcut_analysis_%1.json")
                             .arg(QDateTime::currentMSecsSinceEpoch());
    m_outPath = QDir::tempPath() + QLatin1Char('/') + uniq;

    m_stdoutBuf.clear();
    m_stderrTail.clear();
    m_cancelled = false;

    QStringList args = {
        QStringLiteral("-m"), QStringLiteral("censorcut.analyze"),
        QStringLiteral("--input"), inputPath,
        QStringLiteral("--out"),   m_outPath,
    };

    m_proc.setWorkingDirectory(m_packageDir);
    m_proc.setProgram(m_pythonPath);
    m_proc.setArguments(args);
    m_proc.start();
    if (!m_proc.waitForStarted(5000)) {
        emit failed(QStringLiteral("python failed to start: %1").arg(m_proc.errorString()));
        return false;
    }
    return true;
}

void AnalysisController::cancel()
{
    if (!isRunning()) return;
    m_cancelled = true;
    m_proc.kill();
    m_proc.waitForFinished(2000);
}

void AnalysisController::onStdoutReady()
{
    m_stdoutBuf.append(m_proc.readAllStandardOutput());
    int idx;
    while ((idx = m_stdoutBuf.indexOf('\n')) >= 0) {
        QByteArray line = m_stdoutBuf.left(idx);
        m_stdoutBuf.remove(0, idx + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (!line.isEmpty()) parseStdoutLine(line);
    }
}

void AnalysisController::onStderrReady()
{
    appendBounded(m_stderrTail, m_proc.readAllStandardError());
}

void AnalysisController::parseStdoutLine(const QByteArray& line)
{
    // Expected: "PROGRESS <fraction> phase=<name>"
    if (!line.startsWith("PROGRESS")) return;
    const QList<QByteArray> parts = line.split(' ');
    if (parts.size() < 2) return;
    bool ok = false;
    const double frac = parts.at(1).toDouble(&ok);
    if (!ok) return;
    emit progressChanged(frac);
    for (int i = 2; i < parts.size(); ++i) {
        if (parts.at(i).startsWith("phase=")) {
            const QString phase = QString::fromUtf8(parts.at(i).mid(6));
            emit phaseChanged(phase);
        }
    }
}

void AnalysisController::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    onStdoutReady();
    appendBounded(m_stderrTail, m_proc.readAllStandardError());

    if (m_cancelled) {
        QFile::remove(m_outPath);
        emit failed(QStringLiteral("Cancelled."));
        return;
    }
    if (status != QProcess::NormalExit || exitCode != 0) {
        QFile::remove(m_outPath);
        emit failed(QStringLiteral("Analyzer exited with code %1.\n\n%2")
                        .arg(exitCode).arg(QString::fromUtf8(m_stderrTail.right(2000))));
        return;
    }

    QFile f(m_outPath);
    if (!f.open(QIODevice::ReadOnly)) {
        emit failed(QStringLiteral("Could not read analyzer output: %1").arg(m_outPath));
        return;
    }
    const QByteArray bytes = f.readAll();
    f.close();
    QFile::remove(m_outPath);

    QString parseErr;
    AnalysisResult result = parseAnalysisResultJson(bytes, &parseErr);
    if (!parseErr.isEmpty()) {
        emit failed(parseErr);
        return;
    }
    emit completed(result);
}

void AnalysisController::onErrorOccurred(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        emit failed(QStringLiteral("python failed to start: %1").arg(m_proc.errorString()));
    }
}

} // namespace censorcut
