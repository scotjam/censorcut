#pragma once

#include "core/AgeProfile.h"
#include "core/AnalysisResult.h"

#include <QString>
#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace censorcut {

class AnalysisController;
class MarkerModel;

/// The right-pane analyzer UI: age selector, profile label, Run button,
/// progress, and a result summary. Owns an AnalysisController, runs it
/// against the current movie, and pushes Source::Suggested markers into
/// the supplied MarkerModel.
class AnalyzerPanel : public QWidget {
    Q_OBJECT
public:
    explicit AnalyzerPanel(MarkerModel* markers, QWidget* parent = nullptr);

    /// Set the current source video and its known duration. Pass an empty
    /// path to disable Run.
    void setMovie(const QString& sourcePath, qint64 durationMs);

    /// The age the user has selected. Other code can use AgeProfile::forAge
    /// to translate to a profile.
    int  selectedAge() const;
    void setSelectedAge(int age);

signals:
    /// Emitted when the user changes the age. Carries the chosen age (the
    /// caller can re-derive the AgeProfile if needed).
    void ageChanged(int age);

private slots:
    void onRunClicked();
    void onCancelClicked();
    void onProgress(double fraction);
    void onPhase(const QString& phase);
    void onCompleted(const AnalysisResult& result);
    void onFailed(const QString& reason);

private:
    void setRunning(bool running);
    void describeProfile();

    MarkerModel*       m_markers    = nullptr;
    AnalysisController* m_controller = nullptr;

    QString m_sourcePath;
    qint64  m_durationMs = 0;
    int     m_age = 8;

    QSpinBox*    m_ageSpin   = nullptr;
    QLabel*      m_profileLabel = nullptr;
    QPushButton* m_runBtn    = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel*      m_phaseLabel = nullptr;
    QLabel*      m_summaryLabel = nullptr;
};

} // namespace censorcut
