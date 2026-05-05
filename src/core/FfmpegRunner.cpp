#include "FfmpegRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>

namespace censorcut {

namespace {

constexpr int kStderrTailMax = 64 * 1024;  // keep the last 64 KB

void appendBounded(QByteArray& tail, const QByteArray& chunk)
{
    tail.append(chunk);
    if (tail.size() > kStderrTailMax) {
        tail.remove(0, tail.size() - kStderrTailMax);
    }
}

} // namespace

FfmpegRunner::FfmpegRunner(QObject* parent)
    : QObject(parent)
{
    connect(&m_proc, &QProcess::readyReadStandardOutput,
            this, &FfmpegRunner::onStdoutReady);
    connect(&m_proc, &QProcess::readyReadStandardError,
            this, &FfmpegRunner::onStderrReady);
    connect(&m_proc, &QProcess::finished,
            this, &FfmpegRunner::onProcessFinished);
    connect(&m_proc, &QProcess::errorOccurred,
            this, &FfmpegRunner::onErrorOccurred);
}

FfmpegRunner::~FfmpegRunner()
{
    if (m_proc.state() != QProcess::NotRunning) {
        m_proc.kill();
        m_proc.waitForFinished(2000);
    }
}

QString FfmpegRunner::locateFfmpeg()
{
    // 1. Bundled under <app>/third_party/ffmpeg/bin/
    const QStringList candidates = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/third_party/ffmpeg/bin/ffmpeg.exe"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/third_party/ffmpeg/bin/ffmpeg"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/ffmpeg.exe"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/ffmpeg"),
    };
    for (const auto& path : candidates) {
        if (QFileInfo::exists(path)) return QDir::toNativeSeparators(path);
    }

    // 2. PATH
    const QString sys = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!sys.isEmpty()) return sys;

    return {};
}

QString FfmpegRunner::locateFfprobe()
{
    const QStringList candidates = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/third_party/ffmpeg/bin/ffprobe.exe"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/third_party/ffmpeg/bin/ffprobe"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/ffprobe.exe"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/ffprobe"),
    };
    for (const auto& path : candidates) {
        if (QFileInfo::exists(path)) return QDir::toNativeSeparators(path);
    }
    const QString sys = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (!sys.isEmpty()) return sys;
    return {};
}

void FfmpegRunner::setFfmpegPath(const QString& path)
{
    m_ffmpegPath = path;
}

QString FfmpegRunner::ffmpegPath() const
{
    return m_ffmpegPath;
}

void FfmpegRunner::setExpectedOutputDurationMs(qint64 ms)
{
    m_expectedDurationMs = ms;
}

bool FfmpegRunner::start(const QStringList& args)
{
    if (isRunning()) return false;

    QString exe = m_ffmpegPath.isEmpty() ? locateFfmpeg() : m_ffmpegPath;
    if (exe.isEmpty()) {
        emit failed(QStringLiteral("ffmpeg executable not found. Install ffmpeg or "
                                   "place it under third_party/ffmpeg/bin/."));
        return false;
    }
    m_ffmpegPath = exe;

    m_stdoutBuf.clear();
    m_stderrTail.clear();
    m_lastOutTimeUs    = 0;
    m_lastReportedFrac = -1.0;
    m_cancelled        = false;

    m_proc.setProgram(exe);
    m_proc.setArguments(args);
    m_proc.start();
    if (!m_proc.waitForStarted(5000)) {
        emit failed(QStringLiteral("ffmpeg failed to start: %1").arg(m_proc.errorString()));
        return false;
    }
    return true;
}

void FfmpegRunner::cancel()
{
    if (!isRunning()) return;
    m_cancelled = true;
    m_proc.kill();
    m_proc.waitForFinished(2000);
}

bool FfmpegRunner::isRunning() const
{
    return m_proc.state() != QProcess::NotRunning;
}

void FfmpegRunner::onStdoutReady()
{
    m_stdoutBuf.append(m_proc.readAllStandardOutput());
    // Process complete lines. ffmpeg uses '\n' as the line terminator on all
    // platforms when emitting -progress output.
    int idx;
    while ((idx = m_stdoutBuf.indexOf('\n')) >= 0) {
        QByteArray line = m_stdoutBuf.left(idx);
        m_stdoutBuf.remove(0, idx + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (!line.isEmpty()) parseProgressLine(line);
    }
}

void FfmpegRunner::onStderrReady()
{
    appendBounded(m_stderrTail, m_proc.readAllStandardError());
}

void FfmpegRunner::parseProgressLine(const QByteArray& line)
{
    const int eq = line.indexOf('=');
    if (eq <= 0) return;
    const QByteArray key   = line.left(eq).trimmed();
    const QByteArray value = line.mid(eq + 1).trimmed();

    if (key == "out_time_ms" || key == "out_time_us") {
        // Despite the name, ffmpeg reports microseconds in out_time_ms.
        bool ok = false;
        const qint64 us = value.toLongLong(&ok);
        if (ok && us >= 0) m_lastOutTimeUs = us;
    } else if (key == "progress") {
        if (m_expectedDurationMs > 0) {
            const double frac = std::clamp(
                double(m_lastOutTimeUs) / 1000.0 / double(m_expectedDurationMs),
                0.0, 1.0);
            // De-dupe small wiggles: only emit if it moved by >=0.1% or hit end.
            const bool finalTick = (value == "end");
            if (finalTick || std::abs(frac - m_lastReportedFrac) >= 0.001) {
                m_lastReportedFrac = frac;
                emit progressChanged(finalTick ? 1.0 : frac);
            }
        }
    }
}

void FfmpegRunner::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    // Drain any remaining stdout/stderr.
    onStdoutReady();
    appendBounded(m_stderrTail, m_proc.readAllStandardError());

    const bool ok = !m_cancelled
                  && status == QProcess::NormalExit
                  && exitCode == 0;
    if (m_cancelled) {
        emit finished(false, QStringLiteral("Cancelled."));
    } else {
        emit finished(ok, QString::fromUtf8(m_stderrTail));
    }
}

void FfmpegRunner::onErrorOccurred(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        emit failed(QStringLiteral("ffmpeg failed to start: %1").arg(m_proc.errorString()));
    }
    // Other QProcess errors (Crashed, Timedout, ReadError, WriteError) will
    // also surface as a non-zero exit; we let onProcessFinished handle them.
}

} // namespace censorcut
