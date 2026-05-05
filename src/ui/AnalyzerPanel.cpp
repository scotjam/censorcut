#include "AnalyzerPanel.h"

#include "core/AnalysisController.h"
#include "core/Marker.h"
#include "core/MarkerModel.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace censorcut {

AnalyzerPanel::AnalyzerPanel(MarkerModel* markers, QWidget* parent)
    : QWidget(parent), m_markers(markers)
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

    auto* note = new QLabel(
        tr("M3 detects loud spikes after quiet stretches (jump-scare-like). "
           "More categories land in M4 (YAMNet) and beyond."), this);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#777; font-size: 10pt;"));
    main->addWidget(note);

    main->addStretch(1);

    m_controller = new AnalysisController(this);
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
    // After the user presses Enter or tabs out, give focus back to the top
    // level so transport keys (Space, ←/→, J/K/L) work again.
    connect(m_ageSpin, &QSpinBox::editingFinished, this, [this]{
        if (auto* w = window()) w->setFocus(Qt::OtherFocusReason);
    });

    describeProfile();
    setMovie(QString(), 0);
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

    m_summaryLabel->setText(tr("Found %1 suggestion(s) covering %2 of runtime. "
                               "Review and Confirm/Reject in the marker list or timeline.")
                                .arg(added)
                                .arg(QString::number(pct, 'f', 1) + QStringLiteral("%")));
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

} // namespace censorcut
