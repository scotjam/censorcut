#include "ExportController.h"

#include "FfmpegRunner.h"
#include "SubtitleProcessor.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTextStream>

#include <algorithm>

namespace censorcut {

namespace {
constexpr double kEncodeShare = 0.85;  // first 85% of progress is per-segment encoding
constexpr double kConcatShare = 0.10;  // 10% concat
constexpr double kSubMuxShare = 0.05;  // last 5% subtitle re-mux (skipped if no subs)

bool isTextSubtitleCodec(const QString& codec)
{
    static const QSet<QString> kTextCodecs = {
        QStringLiteral("subrip"),
        QStringLiteral("srt"),
        QStringLiteral("ass"),
        QStringLiteral("ssa"),
        QStringLiteral("mov_text"),
        QStringLiteral("webvtt"),
        QStringLiteral("text"),
    };
    return kTextCodecs.contains(codec.toLower());
}

/// Map ISO 639-1 (2-letter) to 639-2 (3-letter) for the most common
/// languages. Pass-through for codes already 3 chars or for unknown
/// inputs — both ffmpeg and modern containers are tolerant about that.
QString normalizeLanguageCode(const QString& raw)
{
    if (raw.isEmpty()) return raw;
    const QString lc = raw.toLower();
    if (lc.size() != 2) return lc;
    static const QHash<QString, QString> map = {
        {"en","eng"},{"es","spa"},{"fr","fra"},{"de","deu"},
        {"it","ita"},{"pt","por"},{"ja","jpn"},{"zh","zho"},
        {"ru","rus"},{"ar","ara"},{"ko","kor"},{"nl","nld"},
        {"hi","hin"},{"tr","tur"},{"pl","pol"},{"sv","swe"},
        {"no","nor"},{"da","dan"},{"fi","fin"},{"cs","ces"},
        {"el","ell"},{"he","heb"},{"hu","hun"},{"id","ind"},
        {"ro","ron"},{"th","tha"},{"vi","vie"},{"uk","ukr"},
        {"bg","bul"},{"hr","hrv"},{"sk","slk"},{"sl","slv"},
        {"et","est"},{"lv","lav"},{"lt","lit"},{"ca","cat"},
        {"ms","msa"},{"fa","fas"},{"bn","ben"},{"ta","tam"},
        {"te","tel"},{"ml","mal"},{"mr","mar"},{"ur","urd"},
    };
    return map.value(lc, lc);
}

QString languageDisplayName(const QString& iso2or3)
{
    const QString c = iso2or3.toLower();
    static const QHash<QString, QString> map = {
        {"eng","English"},   {"spa","Spanish"},   {"fra","French"},
        {"deu","German"},    {"ita","Italian"},   {"por","Portuguese"},
        {"jpn","Japanese"},  {"zho","Chinese"},   {"chi","Chinese"},
        {"rus","Russian"},   {"ara","Arabic"},    {"kor","Korean"},
        {"nld","Dutch"},     {"hin","Hindi"},     {"tur","Turkish"},
        {"pol","Polish"},    {"swe","Swedish"},   {"nor","Norwegian"},
        {"dan","Danish"},    {"fin","Finnish"},   {"ces","Czech"},
        {"ell","Greek"},     {"heb","Hebrew"},    {"hun","Hungarian"},
        {"ind","Indonesian"},{"ron","Romanian"},  {"tha","Thai"},
        {"vie","Vietnamese"},{"ukr","Ukrainian"}, {"bul","Bulgarian"},
        {"hrv","Croatian"},  {"slk","Slovak"},    {"slv","Slovenian"},
        {"est","Estonian"},  {"lav","Latvian"},   {"lit","Lithuanian"},
        {"cat","Catalan"},   {"msa","Malay"},     {"fas","Persian"},
        {"ben","Bengali"},   {"tam","Tamil"},     {"tel","Telugu"},
        {"mal","Malayalam"}, {"mar","Marathi"},   {"urd","Urdu"},
    };
    return map.value(c, QString{});
}

/// Parse a sidecar filename like "Movie.en.forced.srt" into language +
/// title hints. Returns (language ISO 639-2, title) — both may be empty.
QPair<QString, QString> parseSidecarLanguage(const QString& sourceStem,
                                             const QString& fileName)
{
    QString rest = fileName;
    if (rest.startsWith(sourceStem)) rest = rest.mid(sourceStem.size());
    if (!rest.isEmpty() && rest.at(0) == QLatin1Char('.')) rest = rest.mid(1);
    const int lastDot = rest.lastIndexOf(QLatin1Char('.'));
    if (lastDot > 0) rest = rest.left(lastDot);  // strip the file extension

    if (rest.isEmpty()) return {QString{}, QString{}};

    QString lang;
    QStringList titleHints;
    const QStringList parts = rest.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const bool isAlpha = std::all_of(part.cbegin(), part.cend(),
                                         [](QChar c) { return c.isLetter(); });
        if (lang.isEmpty() && isAlpha
            && (part.size() == 2 || part.size() == 3)) {
            lang = normalizeLanguageCode(part);
            continue;
        }
        titleHints.append(part);
    }

    QString title;
    if (!lang.isEmpty()) {
        const QString human = languageDisplayName(lang);
        if (!human.isEmpty()) title = human;
    }
    if (!titleHints.isEmpty()) {
        const QString hint = titleHints.join(QLatin1Char(' '));
        title = title.isEmpty() ? hint : QStringLiteral("%1 (%2)").arg(title, hint);
    }
    return {lang, title};
}

QString subtitleCodecForContainer(const QString& destinationPath)
{
    const QString suffix = QFileInfo(destinationPath).suffix().toLower();
    if (suffix == QStringLiteral("mp4") || suffix == QStringLiteral("m4v")
        || suffix == QStringLiteral("mov")) {
        return QStringLiteral("mov_text");
    }
    return QStringLiteral("srt");
}
} // namespace

ExportController::ExportController(QObject* parent)
    : QObject(parent)
{
    m_runner = new FfmpegRunner(this);
}

ExportController::~ExportController()
{
    cleanupTempDir();
}

bool ExportController::isRunning() const
{
    return m_phase == Phase::Encoding
        || m_phase == Phase::Concat
        || m_phase == Phase::SubtitleMux;
}

QString ExportController::segmentPath(int index) const
{
    return QStringLiteral("%1/seg%2.mp4").arg(m_tempDir).arg(index, 4, 10, QChar('0'));
}

bool ExportController::start(const Project& project,
                             const QString& destinationPath,
                             ExportQuality quality)
{
    if (isRunning()) return false;

    m_project     = project;
    m_destination = destinationPath;
    m_quality     = quality;
    m_plan        = planExport(project);
    if (!m_plan.ok()) {
        emit failed(m_plan.errorMessage);
        return false;
    }
    m_totalDurationMs = 0;
    for (const auto& s : m_plan.keepSegments) m_totalDurationMs += s.durationMs();
    m_doneDurationMs    = 0;
    m_currentSegmentIdx = 0;
    m_cancelRequested   = false;
    m_textSubtitleStreams.clear();
    m_cutSubtitleTracks.clear();
    m_tempMuxedOutput.clear();

    const QString uniq = QStringLiteral("censorcut_export_%1")
                             .arg(QDateTime::currentMSecsSinceEpoch());
    m_tempDir = QDir::tempPath() + QLatin1Char('/') + uniq;
    if (!QDir().mkpath(m_tempDir)) {
        emit failed(QStringLiteral("Could not create temp directory: %1").arg(m_tempDir));
        return false;
    }

    // Subtitle prep is synchronous and runs before the (slow) encode phase
    // so any failure surfaces immediately. We pick up two sources:
    //   1. Embedded text subtitle streams in the source container.
    //   2. Sidecar files next to the source: <movie>.srt, <movie>.en.srt,
    //      .ass, .ssa, .vtt.
    emit phaseChanged(QStringLiteral("Looking for subtitles…"));
    probeTextSubtitles();
    extractAndCutSubtitles();
    pickUpSidecarSubtitles();
    if (!m_cutSubtitleTracks.isEmpty()) {
        emit phaseChanged(QStringLiteral("Re-timed %1 subtitle track(s).")
                              .arg(m_cutSubtitleTracks.size()));
    }

    disconnect(m_runner, nullptr, this, nullptr);
    connect(m_runner, &FfmpegRunner::progressChanged,
            this, &ExportController::onSegmentProgress);
    connect(m_runner, &FfmpegRunner::finished,
            this, &ExportController::onSegmentFinished);
    connect(m_runner, &FfmpegRunner::failed,
            this, &ExportController::onRunnerFailed);

    m_phase = Phase::Encoding;
    encodeNextSegment();
    return true;
}

void ExportController::encodeNextSegment()
{
    const auto& seg = m_plan.keepSegments.at(m_currentSegmentIdx);

    ExportArgsOptions opts;
    opts.sourcePath = m_project.sourceFile;
    opts.outputPath = segmentPath(m_currentSegmentIdx);
    opts.segment    = seg;
    opts.quality    = m_quality;
    // For the v1 export we don't probe ffprobe yet — leave fps unset so
    // ffmpeg keeps the source rate. -vsync cfr in the args still requests
    // a constant frame rate so concat boundaries don't drift.

    emit phaseChanged(QStringLiteral("Encoding segment %1 of %2 — %3 quality")
                          .arg(m_currentSegmentIdx + 1)
                          .arg(m_plan.keepSegments.size())
                          .arg(m_quality == ExportQuality::Accurate
                                   ? QStringLiteral("frame-accurate")
                                   : QStringLiteral("fast")));

    m_runner->setExpectedOutputDurationMs(seg.durationMs());
    if (!m_runner->start(buildSegmentEncodeArgs(opts))) {
        // failed() will fire via onRunnerFailed
    }
}

void ExportController::onSegmentProgress(double fraction)
{
    if (m_phase != Phase::Encoding) return;
    const auto& seg = m_plan.keepSegments.at(m_currentSegmentIdx);
    const qint64 segDoneMs   = qint64(fraction * double(seg.durationMs()));
    const qint64 totalDoneMs = m_doneDurationMs + segDoneMs;
    const double frac = m_totalDurationMs > 0
        ? std::clamp(double(totalDoneMs) / double(m_totalDurationMs), 0.0, 1.0)
        : 0.0;
    emit progressChanged(frac * kEncodeShare);
}

void ExportController::onSegmentFinished(bool ok, const QString& stderrTail)
{
    if (m_phase != Phase::Encoding) return;
    if (m_cancelRequested) { abortWith(QStringLiteral("Cancelled.")); return; }
    if (!ok) {
        abortWith(QStringLiteral("ffmpeg failed while encoding segment %1.\n\n%2")
                      .arg(m_currentSegmentIdx + 1).arg(stderrTail.right(2000)));
        return;
    }
    m_doneDurationMs += m_plan.keepSegments.at(m_currentSegmentIdx).durationMs();
    ++m_currentSegmentIdx;
    if (m_currentSegmentIdx < m_plan.keepSegments.size()) {
        encodeNextSegment();
    } else {
        runConcat();
    }
}

void ExportController::runConcat()
{
    const QString listPath = m_tempDir + QStringLiteral("/list.txt");
    {
        QFile lf(listPath);
        if (!lf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            abortWith(QStringLiteral("Could not write concat list: %1").arg(listPath));
            return;
        }
        QTextStream out(&lf);
        for (int i = 0; i < m_plan.keepSegments.size(); ++i) {
            // Relative names; concat demuxer resolves them against the list
            // file's directory.
            out << "file '" << QStringLiteral("seg%1.mp4").arg(i, 4, 10, QChar('0')) << "'\n";
        }
    }

    const QString suffix = QFileInfo(m_destination).suffix();
    m_tempOutput = m_tempDir + QStringLiteral("/output.")
                 + (suffix.isEmpty() ? QStringLiteral("mp4") : suffix);

    disconnect(m_runner, nullptr, this, nullptr);
    connect(m_runner, &FfmpegRunner::progressChanged,
            this, &ExportController::onConcatProgress);
    connect(m_runner, &FfmpegRunner::finished,
            this, &ExportController::onConcatFinished);
    connect(m_runner, &FfmpegRunner::failed,
            this, &ExportController::onRunnerFailed);

    emit phaseChanged(QStringLiteral("Combining %1 segments…").arg(m_plan.keepSegments.size()));

    m_phase = Phase::Concat;
    m_runner->setExpectedOutputDurationMs(m_totalDurationMs);
    if (!m_runner->start(buildConcatArgs(listPath, m_tempOutput))) {
        // failed() fires via onRunnerFailed
    }
}

void ExportController::onConcatProgress(double fraction)
{
    if (m_phase != Phase::Concat) return;
    emit progressChanged(std::clamp(kEncodeShare + fraction * kConcatShare, 0.0, 1.0));
}

void ExportController::onConcatFinished(bool ok, const QString& stderrTail)
{
    if (m_phase != Phase::Concat) return;
    if (m_cancelRequested) { abortWith(QStringLiteral("Cancelled.")); return; }
    if (!ok) {
        abortWith(QStringLiteral("ffmpeg failed while combining segments.\n\n%1")
                      .arg(stderrTail.right(2000)));
        return;
    }

    // If we collected re-timed subtitle tracks, mux them in before the move.
    if (!m_cutSubtitleTracks.isEmpty()) {
        runSubtitleMux();
        return;
    }
    finalizeOutput();
}

void ExportController::onSubtitleMuxProgress(double fraction)
{
    if (m_phase != Phase::SubtitleMux) return;
    emit progressChanged(std::clamp(kEncodeShare + kConcatShare + fraction * kSubMuxShare,
                                    0.0, 1.0));
}

void ExportController::onSubtitleMuxFinished(bool ok, const QString& stderrTail)
{
    if (m_phase != Phase::SubtitleMux) return;
    if (m_cancelRequested) { abortWith(QStringLiteral("Cancelled.")); return; }
    if (!ok) {
        // Soft-fail on subtitle mux: keep the subtitle-less output instead of
        // failing the whole job. Surfaced as a phase change for visibility.
        emit phaseChanged(QStringLiteral(
            "Subtitle mux failed — saving the output without subtitles. (%1)")
                .arg(stderrTail.right(400)));
        QFile::remove(m_tempMuxedOutput);
        m_tempMuxedOutput.clear();
    }
    finalizeOutput();
}

void ExportController::finalizeOutput()
{
    // Pick the file we'll move into place: subtitle-muxed if we have one,
    // otherwise the concat output.
    const QString sourceForMove = m_tempMuxedOutput.isEmpty()
                                    ? m_tempOutput : m_tempMuxedOutput;

    if (QFile::exists(m_destination)) {
        if (!QFile::remove(m_destination)) {
            abortWith(QStringLiteral("Destination exists and could not be removed: %1")
                          .arg(m_destination));
            return;
        }
    }
    if (!QFile::rename(sourceForMove, m_destination)) {
        if (!QFile::copy(sourceForMove, m_destination)) {
            abortWith(QStringLiteral("Could not move output to %1").arg(m_destination));
            return;
        }
        QFile::remove(sourceForMove);
    }

    cleanupTempDir();
    m_phase = Phase::Done;
    emit progressChanged(1.0);
    emit completed(m_destination);
}

void ExportController::onRunnerFailed(const QString& reason)
{
    abortWith(reason);
}

void ExportController::runSubtitleMux()
{
    const QString suffix = QFileInfo(m_destination).suffix();
    m_tempMuxedOutput = m_tempDir + QStringLiteral("/output_with_subs.")
                      + (suffix.isEmpty() ? QStringLiteral("mp4") : suffix);

    QStringList args;
    args << QStringLiteral("-y") << QStringLiteral("-hide_banner");
    // Concat output is the first input.
    args << QStringLiteral("-i") << m_tempOutput;
    // Each re-timed SRT becomes another input.
    for (const CutSubtitleTrack& t : m_cutSubtitleTracks) {
        args << QStringLiteral("-i") << t.srtPath;
    }
    // Take everything from the concat output (video + audio + chapters etc.).
    args << QStringLiteral("-map") << QStringLiteral("0");
    // Then map each SRT input as a subtitle stream.
    for (int i = 0; i < m_cutSubtitleTracks.size(); ++i) {
        args << QStringLiteral("-map") << QString::number(i + 1);
    }
    args << QStringLiteral("-c") << QStringLiteral("copy");
    args << QStringLiteral("-c:s") << subtitleCodecForContainer(m_destination);

    // Stamp language + title onto each subtitle output stream. The
    // -metadata:s:s:N selector targets the Nth subtitle stream in the
    // OUTPUT (zero-indexed). Since we map source 0 first (which carries
    // its own subtitle streams, if any) and then our extracted SRTs in
    // order, "our" subtitle outputs come after the passed-through ones.
    // The concat output is built from re-encoded segments and won't
    // carry source subtitle streams (we strip them when re-encoding),
    // so the mapping is 0..N-1 of just our SRTs.
    for (int i = 0; i < m_cutSubtitleTracks.size(); ++i) {
        const auto& t = m_cutSubtitleTracks.at(i);
        if (!t.language.isEmpty()) {
            args << QStringLiteral("-metadata:s:s:%1").arg(i)
                 << QStringLiteral("language=%1").arg(t.language);
        }
        if (!t.title.isEmpty()) {
            args << QStringLiteral("-metadata:s:s:%1").arg(i)
                 << QStringLiteral("title=%1").arg(t.title);
        }
    }

    args << QStringLiteral("-progress") << QStringLiteral("pipe:1");
    args << m_tempMuxedOutput;

    disconnect(m_runner, nullptr, this, nullptr);
    connect(m_runner, &FfmpegRunner::progressChanged,
            this, &ExportController::onSubtitleMuxProgress);
    connect(m_runner, &FfmpegRunner::finished,
            this, &ExportController::onSubtitleMuxFinished);
    connect(m_runner, &FfmpegRunner::failed,
            this, &ExportController::onRunnerFailed);

    emit phaseChanged(QStringLiteral("Adding %1 retimed subtitle track(s)…")
                          .arg(m_cutSubtitleTracks.size()));
    m_phase = Phase::SubtitleMux;
    m_runner->setExpectedOutputDurationMs(m_totalDurationMs);
    if (!m_runner->start(args)) {
        // failed() fires via onRunnerFailed
    }
}

void ExportController::probeTextSubtitles()
{
    const QString ffprobe = FfmpegRunner::locateFfprobe();
    if (ffprobe.isEmpty()) return;  // fall back silently to no-subs

    QProcess proc;
    proc.start(ffprobe, {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-select_streams"), QStringLiteral("s"),
        QStringLiteral("-show_entries"),
        QStringLiteral("stream=index,codec_name:stream_tags=language,title"),
        QStringLiteral("-of"), QStringLiteral("json"),
        m_project.sourceFile,
    });
    if (!proc.waitForFinished(15000)) {
        proc.kill();
        return;
    }
    const QByteArray out = proc.readAllStandardOutput();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(out, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    const QJsonArray streams = doc.object().value(QStringLiteral("streams")).toArray();
    int skippedImage = 0;
    for (const QJsonValue& v : streams) {
        const QJsonObject o = v.toObject();
        const int idx = o.value(QStringLiteral("index")).toInt(-1);
        const QString codec = o.value(QStringLiteral("codec_name")).toString();
        if (idx < 0) continue;
        if (!isTextSubtitleCodec(codec)) { ++skippedImage; continue; }

        SubtitleStreamInfo info;
        info.index = idx;
        info.codec = codec;
        const QJsonObject tags = o.value(QStringLiteral("tags")).toObject();
        info.language = normalizeLanguageCode(
            tags.value(QStringLiteral("language")).toString());
        info.title = tags.value(QStringLiteral("title")).toString();
        // If we have a language code but no title, synthesize one for
        // friendlier display in players that show titles.
        if (info.title.isEmpty() && !info.language.isEmpty()) {
            const QString human = languageDisplayName(info.language);
            if (!human.isEmpty()) info.title = human;
        }
        m_textSubtitleStreams.append(info);
    }
    if (skippedImage > 0) {
        emit phaseChanged(QStringLiteral(
            "Skipped %1 image-based subtitle track(s) — only text subs can be re-timed.")
                .arg(skippedImage));
    }
}

bool ExportController::processOneSubtitleSource(const QString& ffmpegPath,
                                                const QStringList& inputArgs,
                                                int ordinal,
                                                const QString& language,
                                                const QString& title)
{
    const QString rawSrt = m_tempDir
        + QStringLiteral("/source_sub_%1.srt").arg(ordinal);
    const QString cutSrt = m_tempDir
        + QStringLiteral("/cut_sub_%1.srt").arg(ordinal);

    QStringList args = inputArgs;
    args << QStringLiteral("-c:s") << QStringLiteral("srt")
         << QStringLiteral("-f")   << QStringLiteral("srt")
         << rawSrt;

    QProcess extract;
    extract.start(ffmpegPath, args);
    if (!extract.waitForFinished(60000) || extract.exitCode() != 0) {
        extract.kill();
        return false;
    }

    QString readErr;
    const QList<SubtitleEntry> src = readSrtFile(rawSrt, &readErr);
    if (src.isEmpty()) return false;

    const QList<SubtitleEntry> cut = applyKeepSegments(src, m_plan.keepSegments);
    if (cut.isEmpty()) return false;

    QString writeErr;
    if (!writeSrtFile(cutSrt, cut, &writeErr)) return false;

    CutSubtitleTrack track;
    track.srtPath  = cutSrt;
    track.language = language;
    track.title    = title;
    m_cutSubtitleTracks.append(track);
    return true;
}

void ExportController::extractAndCutSubtitles()
{
    const QString ffmpeg = FfmpegRunner::locateFfmpeg();
    if (ffmpeg.isEmpty()) return;

    int ordinal = m_cutSubtitleTracks.size();
    for (const SubtitleStreamInfo& info : m_textSubtitleStreams) {
        const QStringList inputArgs = {
            QStringLiteral("-y"), QStringLiteral("-hide_banner"),
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-i"), m_project.sourceFile,
            QStringLiteral("-map"), QStringLiteral("0:%1").arg(info.index),
        };
        processOneSubtitleSource(ffmpeg, inputArgs, ordinal,
                                 info.language, info.title);
        ++ordinal;
    }
}

void ExportController::pickUpSidecarSubtitles()
{
    const QString ffmpeg = FfmpegRunner::locateFfmpeg();
    if (ffmpeg.isEmpty()) return;

    const QFileInfo srcInfo(m_project.sourceFile);
    const QDir      srcDir   = srcInfo.absoluteDir();
    const QString   stem     = srcInfo.completeBaseName();

    // Look for siblings matching <stem>*.{srt,ass,ssa,vtt}. The wildcard
    // catches both the bare form (<stem>.srt) and language-suffixed forms
    // like <stem>.en.srt.
    static const QStringList kExtensions = { QStringLiteral("srt"),
                                              QStringLiteral("ass"),
                                              QStringLiteral("ssa"),
                                              QStringLiteral("vtt") };
    QStringList nameFilters;
    for (const QString& ext : kExtensions) {
        nameFilters << QStringLiteral("%1*.%2").arg(stem, ext);
    }

    const QFileInfoList matches = srcDir.entryInfoList(
        nameFilters, QDir::Files | QDir::NoSymLinks, QDir::Name);

    int ordinal = m_cutSubtitleTracks.size();
    for (const QFileInfo& fi : matches) {
        // Don't pick up the source file itself (it shouldn't match a
        // subtitle extension, but be safe).
        if (fi.absoluteFilePath() == srcInfo.absoluteFilePath()) continue;

        const QStringList inputArgs = {
            QStringLiteral("-y"), QStringLiteral("-hide_banner"),
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-i"), fi.absoluteFilePath(),
        };
        const auto langTitle = parseSidecarLanguage(stem, fi.fileName());
        if (processOneSubtitleSource(ffmpeg, inputArgs, ordinal,
                                     langTitle.first, langTitle.second)) {
            ++ordinal;
        }
    }
}

void ExportController::cancel()
{
    if (!isRunning()) return;
    m_cancelRequested = true;
    if (m_runner->isRunning()) m_runner->cancel();
    // The runner's finished() will fire via onSegmentFinished/onConcatFinished
    // which will see m_cancelRequested and call abortWith("Cancelled.").
}

void ExportController::abortWith(const QString& reason)
{
    cleanupTempDir();
    m_phase = Phase::Idle;
    emit failed(reason);
}

void ExportController::cleanupTempDir()
{
    if (m_tempDir.isEmpty()) return;
    QDir(m_tempDir).removeRecursively();
    m_tempDir.clear();
    m_tempOutput.clear();
}

} // namespace censorcut
