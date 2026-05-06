#include "AnalyzerPanel.h"

#include "core/AnalysisController.h"
#include "core/FeedbackStore.h"
#include "core/Marker.h"
#include "core/MarkerModel.h"
#include "core/PlaybackController.h"

#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace censorcut {

namespace {

QString formatTimeShort(qint64 ms)
{
    if (ms < 0) ms = 0;
    const qint64 sec = ms / 1000;
    const int h = int(sec / 3600);
    const int m = int((sec % 3600) / 60);
    const int s = int(sec % 60);
    if (h > 0) return QStringLiteral("%1:%2:%3")
                   .arg(h).arg(m, 2, 10, QLatin1Char('0'))
                   .arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

} // namespace

AnalyzerPanel::AnalyzerPanel(MarkerModel* markers,
                             PlaybackController* playback,
                             FeedbackStore* feedback,
                             QWidget* parent)
    : QWidget(parent), m_markers(markers), m_playback(playback),
      m_feedback(feedback)
{
    auto* main = new QVBoxLayout(this);
    main->setContentsMargins(12, 12, 12, 12);

    auto* form = new QFormLayout;
    m_ageSpin = new QSpinBox(this);
    m_ageSpin->setRange(3, 18);
    m_ageSpin->setValue(m_age);
    m_ageSpin->setSuffix(tr(" years"));
    form->addRow(tr("Watching with age"), m_ageSpin);

    m_profileLabel = new QLabel(this);
    m_profileLabel->setWordWrap(true);
    m_profileLabel->setStyleSheet(QStringLiteral("color:#888;"));
    form->addRow(tr("Profile"), m_profileLabel);

    // Sensitivity slider — high = MORE sensitive = MORE suggestions.
    // Slider integer 50..200 -> 0.5x..2.0x sensitivity.
    // Internally we pass --threshold-mul = 1 / sensitivity to the analyzer
    // (so 2.0x sensitivity halves every threshold, 0.5x doubles them).
    auto* sensRow = new QHBoxLayout;
    m_sensitivitySlider = new QSlider(Qt::Horizontal, this);
    m_sensitivitySlider->setRange(50, 200);
    m_sensitivitySlider->setSingleStep(5);
    m_sensitivitySlider->setPageStep(10);
    m_sensitivitySlider->setFocusPolicy(Qt::NoFocus);
    {
        QSettings s;
        const double sens = s.value(QStringLiteral("analyzer/sensitivity"), 1.0).toDouble();
        m_sensitivitySlider->setValue(int(std::clamp(sens, 0.5, 2.0) * 100.0));
    }
    m_sensitivityLabel = new QLabel(this);
    m_sensitivityLabel->setMinimumWidth(180);
    sensRow->addWidget(m_sensitivitySlider, /*stretch=*/1);
    sensRow->addWidget(m_sensitivityLabel);
    auto updateSensLabel = [this]{
        const double sens = m_sensitivitySlider->value() / 100.0;
        QString hint;
        if (sens > 1.15)      hint = tr("more sensitive — finds more");
        else if (sens < 0.85) hint = tr("stricter — finds fewer");
        else                  hint = tr("default");
        m_sensitivityLabel->setText(tr("%1×  (%2)").arg(QString::number(sens, 'f', 2), hint));
    };
    updateSensLabel();
    connect(m_sensitivitySlider, &QSlider::valueChanged, this,
            [this, updateSensLabel](int v) {
        updateSensLabel();
        const double sens = v / 100.0;
        QSettings s;
        s.setValue(QStringLiteral("analyzer/sensitivity"), sens);
        if (m_controller) m_controller->setThresholdMultiplier(1.0 / sens);
    });
    form->addRow(tr("Sensitivity"), sensRow);

    main->addLayout(form);

    auto* btnRow = new QHBoxLayout;
    m_runBtn    = new QPushButton(tr("Run analysis"), this);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_cancelBtn->setVisible(false);
    // NoFocus so clicking these doesn't trap keyboard focus and break the
    // transport keys (Space play, arrows seek, etc.) for the rest of the
    // editing session.
    m_runBtn->setFocusPolicy(Qt::NoFocus);
    m_cancelBtn->setFocusPolicy(Qt::NoFocus);
    btnRow->addWidget(m_runBtn);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addStretch(1);
    main->addLayout(btnRow);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setVisible(false);
    main->addWidget(m_progress);

    m_phaseLabel = new QLabel(this);
    m_phaseLabel->setStyleSheet(QStringLiteral("color:#888;"));
    m_phaseLabel->setVisible(false);
    main->addWidget(m_phaseLabel);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setWordWrap(true);
    main->addWidget(m_summaryLabel);

    // ---- Review pending suggestions section -----------------------------
    m_reviewFrame = new QFrame(this);
    m_reviewFrame->setFrameShape(QFrame::StyledPanel);
    auto* reviewLayout = new QVBoxLayout(m_reviewFrame);
    reviewLayout->setContentsMargins(8, 8, 8, 8);
    auto* reviewHeader = new QLabel(tr("Review pending suggestions"), m_reviewFrame);
    reviewHeader->setStyleSheet(QStringLiteral("font-weight:bold;"));
    reviewLayout->addWidget(reviewHeader);

    m_reviewCount = new QLabel(m_reviewFrame);
    m_reviewCount->setStyleSheet(QStringLiteral("color:#888;"));
    reviewLayout->addWidget(m_reviewCount);

    m_reviewCurrent = new QLabel(m_reviewFrame);
    m_reviewCurrent->setWordWrap(true);
    reviewLayout->addWidget(m_reviewCurrent);

    auto* reviewBtns = new QHBoxLayout;
    m_reviewPrev    = new QPushButton(tr("◀ Prev"),     m_reviewFrame);
    m_reviewReject  = new QPushButton(tr("✗ Reject"),   m_reviewFrame);
    m_reviewConfirm = new QPushButton(tr("✓ Confirm"),  m_reviewFrame);
    m_reviewSkip    = new QPushButton(tr("Skip"),       m_reviewFrame);
    m_reviewNext    = new QPushButton(tr("Next ▶"),     m_reviewFrame);
    for (QPushButton* b : {m_reviewPrev, m_reviewReject, m_reviewConfirm,
                            m_reviewSkip, m_reviewNext}) {
        b->setFocusPolicy(Qt::NoFocus);
        reviewBtns->addWidget(b);
    }
    reviewLayout->addLayout(reviewBtns);
    main->addWidget(m_reviewFrame);

    auto* note = new QLabel(
        tr("Heuristics may miss things, hallucinate, or over-cut — review "
           "every suggestion above before exporting."), this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#777; font-size: 10pt;"));
    main->addWidget(note);

    main->addStretch(1);

    m_controller = new AnalysisController(this);
    {
        QSettings s;
        const double sens = std::clamp(
            s.value(QStringLiteral("analyzer/sensitivity"), 1.0).toDouble(),
            0.25, 4.0);
        m_controller->setThresholdMultiplier(1.0 / sens);
    }
    connect(m_controller, &AnalysisController::progressChanged, this, &AnalyzerPanel::onProgress);
    connect(m_controller, &AnalysisController::phaseChanged,    this, &AnalyzerPanel::onPhase);
    connect(m_controller, &AnalysisController::completed,       this, &AnalyzerPanel::onCompleted);
    connect(m_controller, &AnalysisController::failed,          this, &AnalyzerPanel::onFailed);

    connect(m_runBtn,    &QPushButton::clicked, this, &AnalyzerPanel::onRunClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &AnalyzerPanel::onCancelClicked);
    connect(m_ageSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int age) {
        m_age = age;
        describeProfile();
        emit ageChanged(age);
    });
    connect(m_ageSpin, &QSpinBox::editingFinished, this, [this]{
        if (auto* w = window()) w->setFocus(Qt::OtherFocusReason);
    });

    connect(m_reviewPrev,    &QPushButton::clicked, this, &AnalyzerPanel::onReviewPrev);
    connect(m_reviewReject,  &QPushButton::clicked, this, &AnalyzerPanel::onReviewReject);
    connect(m_reviewConfirm, &QPushButton::clicked, this, &AnalyzerPanel::onReviewConfirm);
    connect(m_reviewSkip,    &QPushButton::clicked, this, &AnalyzerPanel::onReviewNext);
    connect(m_reviewNext,    &QPushButton::clicked, this, &AnalyzerPanel::onReviewNext);

    if (m_playback) {
        connect(m_playback, &PlaybackController::positionChanged,
                this, &AnalyzerPanel::onPositionChanged);
    }
    if (m_markers) {
        connect(m_markers, &MarkerModel::rowsInserted,
                this, &AnalyzerPanel::onMarkersChanged);
        connect(m_markers, &MarkerModel::rowsRemoved,
                this, &AnalyzerPanel::onMarkersChanged);
        connect(m_markers, &MarkerModel::dataChanged,
                this, &AnalyzerPanel::onMarkersChanged);
        connect(m_markers, &MarkerModel::modelReset,
                this, &AnalyzerPanel::onMarkersChanged);
    }

    describeProfile();
    setMovie(QString(), 0);
    refreshReviewUi();
}

void AnalyzerPanel::setMovie(const QString& sourcePath, qint64 durationMs)
{
    m_sourcePath = sourcePath;
    m_durationMs = durationMs;
    const bool haveMovie = !sourcePath.isEmpty() && durationMs > 0;
    m_runBtn->setEnabled(haveMovie);
    if (!haveMovie) {
        m_summaryLabel->setText(tr("Open a movie to analyze it."));
    } else if (m_summaryLabel->text().isEmpty()
               || m_summaryLabel->text().startsWith(tr("Open a movie"))) {
        m_summaryLabel->setText(tr("Click Run analysis to find loud spikes."));
    }
}

int AnalyzerPanel::selectedAge() const
{
    return m_age;
}

void AnalyzerPanel::setSelectedAge(int age)
{
    m_age = age;
    if (m_ageSpin->value() != age) m_ageSpin->setValue(age);
    describeProfile();
}

void AnalyzerPanel::describeProfile()
{
    const AgeProfile p = AgeProfile::forAge(m_age);
    m_profileLabel->setText(p.label);
}

void AnalyzerPanel::onRunClicked()
{
    if (m_sourcePath.isEmpty() || m_durationMs <= 0) return;

    setRunning(true);
    m_progress->setValue(0);
    m_phaseLabel->setText(tr("Starting…"));
    m_summaryLabel->setText(tr("Analyzing — this can take a while for long videos."));

    if (!m_controller->start(m_sourcePath)) {
        // failed() will fire and reset the UI.
    }
}

void AnalyzerPanel::onCancelClicked()
{
    if (m_controller->isRunning()) m_controller->cancel();
}

void AnalyzerPanel::onProgress(double fraction)
{
    m_progress->setValue(int(fraction * 1000));
}

void AnalyzerPanel::onPhase(const QString& phase)
{
    m_phaseLabel->setText(tr("Phase: %1").arg(phase));
}

void AnalyzerPanel::onCompleted(const AnalysisResult& result)
{
    setRunning(false);

    if (m_feedback) m_feedback->setLatestEmbeddings(result.frameEmbeddings);

    int added = 0;
    if (m_markers) {
        for (const auto& s : result.suggestions) {
            Marker m;
            m.startMs    = s.startMs;
            m.endMs      = s.endMs;
            m.category   = s.category;
            m.note       = s.reasons.join(QStringLiteral(" · "));
            m.source     = Source::Suggested;
            m.confidence = s.score;
            m.status     = Status::Pending;
            m_markers->addMarker(m);
            ++added;
        }
    }

    qint64 totalMs = 0;
    for (const auto& s : result.suggestions) totalMs += (s.endMs - s.startMs);
    const double pct = result.durationMs > 0
        ? 100.0 * double(totalMs) / double(result.durationMs) : 0.0;

    QStringList lines;
    lines << tr("Found %1 suggestion(s) covering %2 of runtime.")
                  .arg(added)
                  .arg(QString::number(pct, 'f', 1) + QStringLiteral("%"));
    QStringList detLines;
    if (result.yamnetUsed)  detLines << QStringLiteral("YAMNet");
    if (result.clipUsed)    detLines << QStringLiteral("CLIP");
    if (result.whisperUsed) detLines << QStringLiteral("Whisper");
    if (!detLines.isEmpty())
        lines << tr("Detectors used: %1.").arg(detLines.join(QStringLiteral(", ")));
    if (added == 0 && !result.diagnostics.isEmpty()) {
        // Show the categories closest to firing so the user knows whether
        // to lower Sensitivity or accept "no scary content found here".
        QList<CategoryDiagnostic> sorted = result.diagnostics;
        std::sort(sorted.begin(), sorted.end(),
                  [](const CategoryDiagnostic& a, const CategoryDiagnostic& b) {
                      return (a.peak - a.threshold) > (b.peak - b.threshold);
                  });
        QStringList top;
        for (int i = 0; i < std::min<int>(3, sorted.size()); ++i) {
            const auto& d = sorted.at(i);
            top << QStringLiteral("%1: peak %2 vs %3")
                       .arg(d.category)
                       .arg(QString::number(d.peak, 'f', 2))
                       .arg(QString::number(d.threshold, 'f', 2));
        }
        lines << tr("Closest categories — %1. "
                    "Drag Sensitivity above 1.00× to find more.")
                    .arg(top.join(QStringLiteral("; ")));
    }
    m_summaryLabel->setText(lines.join(QStringLiteral("\n")));
    m_phaseLabel->setText(tr("Done."));
}

void AnalyzerPanel::onFailed(const QString& reason)
{
    setRunning(false);
    m_summaryLabel->setText(tr("Analysis failed: %1").arg(reason));
    m_phaseLabel->setVisible(false);
    m_progress->setVisible(false);
}

void AnalyzerPanel::setRunning(bool running)
{
    m_ageSpin->setEnabled(!running);
    m_runBtn->setVisible(!running);
    m_runBtn->setEnabled(!running && !m_sourcePath.isEmpty());
    m_cancelBtn->setVisible(running);
    m_progress->setVisible(running);
    m_phaseLabel->setVisible(running);
}

// --------------------------------------------------------------------------
// Review-pending workflow
// --------------------------------------------------------------------------

void AnalyzerPanel::onPositionChanged(qint64 ms)
{
    // While reviewing one marker, auto-pause when the playhead reaches the
    // post-roll boundary so the user can decide without scrambling for the
    // pause key.
    if (m_reviewId.isNull() || m_reviewPauseAtMs < 0 || !m_playback) return;
    if (ms < m_reviewPauseAtMs) return;
    m_playback->pause();
    m_reviewPauseAtMs = -1;  // arm-once
}

void AnalyzerPanel::onMarkersChanged()
{
    refreshReviewUi();
}

QUuid AnalyzerPanel::pendingAfter(qint64 referenceMs) const
{
    if (!m_markers) return {};
    QUuid bestId;
    qint64 bestStart = std::numeric_limits<qint64>::max();
    for (const auto& m : m_markers->markers()) {
        if (m.status != Status::Pending) continue;
        if (m.startMs <= referenceMs) continue;
        if (m.startMs < bestStart) { bestStart = m.startMs; bestId = m.id; }
    }
    return bestId;
}

QUuid AnalyzerPanel::pendingBefore(qint64 referenceMs) const
{
    if (!m_markers) return {};
    QUuid bestId;
    qint64 bestStart = std::numeric_limits<qint64>::min();
    for (const auto& m : m_markers->markers()) {
        if (m.status != Status::Pending) continue;
        if (m.startMs >= referenceMs) continue;
        if (m.startMs > bestStart) { bestStart = m.startMs; bestId = m.id; }
    }
    return bestId;
}

void AnalyzerPanel::startReviewFor(const QUuid& id, qint64 newPositionMs)
{
    if (id.isNull() || !m_markers || !m_playback) return;
    auto m = m_markers->findById(id);
    if (!m) return;

    m_reviewId = id;
    const qint64 startWith = std::max<qint64>(0, m->startMs - m_reviewPreRollMs);
    m_reviewPauseAtMs = (m->endMs >= 0)
        ? std::min(m->endMs + m_reviewPostRollMs,
                   m_durationMs > 0 ? m_durationMs : (m->endMs + m_reviewPostRollMs))
        : -1;
    Q_UNUSED(newPositionMs);
    m_playback->seek(startWith);
    m_playback->play();
    refreshReviewUi();
}

void AnalyzerPanel::onReviewNext()
{
    if (!m_markers || !m_playback) return;
    const qint64 ref = m_reviewId.isNull()
        ? m_playback->position()
        : [&]() -> qint64 {
              auto m = m_markers->findById(m_reviewId);
              return m ? m->startMs : m_playback->position();
          }();
    const QUuid next = pendingAfter(ref);
    if (next.isNull()) {
        m_reviewId = QUuid();
        m_reviewPauseAtMs = -1;
        refreshReviewUi();
        return;
    }
    startReviewFor(next);
}

void AnalyzerPanel::onReviewPrev()
{
    if (!m_markers || !m_playback) return;
    const qint64 ref = m_reviewId.isNull()
        ? m_playback->position()
        : [&]() -> qint64 {
              auto m = m_markers->findById(m_reviewId);
              return m ? m->startMs : m_playback->position();
          }();
    const QUuid prev = pendingBefore(ref);
    if (prev.isNull()) return;
    startReviewFor(prev);
}

void AnalyzerPanel::setStatusAndAdvance(int newStatus)
{
    if (m_reviewId.isNull() || !m_markers) {
        // No active review marker — start one, then the caller can decide.
        onReviewNext();
        return;
    }
    auto m = m_markers->findById(m_reviewId);
    if (m) {
        Marker copy = *m;
        copy.status = static_cast<Status>(newStatus);
        m_markers->updateMarkerById(m_reviewId, copy);
        // Record the decision for the local feedback loop. Only Suggested
        // markers contribute (manual markers don't have semantic embeddings).
        if (m_feedback && m->source == Source::Suggested) {
            const auto decision = (copy.status == Status::Confirmed)
                ? FeedbackStore::Decision::Accepted
                : FeedbackStore::Decision::Rejected;
            m_feedback->recordDecision(copy, decision);
        }
    }
    // Use the marker's startMs as the reference for "next" so we don't
    // re-pick the one we just acted on.
    qint64 ref = m ? m->startMs : 0;
    m_reviewId = QUuid();
    m_reviewPauseAtMs = -1;
    const QUuid next = pendingAfter(ref);
    if (!next.isNull()) startReviewFor(next);
    else                refreshReviewUi();
}

void AnalyzerPanel::onReviewConfirm()
{
    setStatusAndAdvance(static_cast<int>(Status::Confirmed));
}

void AnalyzerPanel::onReviewReject()
{
    setStatusAndAdvance(static_cast<int>(Status::Rejected));
}

void AnalyzerPanel::refreshReviewUi()
{
    if (!m_markers) return;
    int pending = 0, total = 0;
    for (const auto& m : m_markers->markers()) {
        if (m.source != Source::Suggested) continue;
        ++total;
        if (m.status == Status::Pending) ++pending;
    }

    const bool show = total > 0;
    m_reviewFrame->setVisible(show);
    if (!show) return;

    m_reviewCount->setText(tr("%1 pending of %2 total suggestion(s)").arg(pending).arg(total));

    QString currentText;
    if (!m_reviewId.isNull()) {
        auto m = m_markers->findById(m_reviewId);
        if (m) {
            currentText = tr("Reviewing: %1   %2 → %3   (score %4)")
                              .arg(m->category)
                              .arg(formatTimeShort(m->startMs))
                              .arg(formatTimeShort(m->endMs))
                              .arg(QString::number(m->confidence, 'f', 2));
        }
    } else if (pending > 0) {
        currentText = tr("Click Next to start reviewing.");
    } else {
        currentText = tr("All suggestions reviewed.");
    }
    m_reviewCurrent->setText(currentText);

    const bool havePending = pending > 0;
    m_reviewConfirm->setEnabled(!m_reviewId.isNull());
    m_reviewReject->setEnabled(!m_reviewId.isNull());
    m_reviewSkip->setEnabled(havePending);
    m_reviewNext->setEnabled(havePending);
    m_reviewPrev->setEnabled(true);  // can always step back
}

} // namespace censorcut
