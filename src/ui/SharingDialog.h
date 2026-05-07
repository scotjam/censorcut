#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;

namespace censorcut {

/// Sharing preferences. Persists toggles via QSettings under "sharing/*".
/// The MainWindow owns whether the censorcut-sync subprocess is actually
/// running; this dialog only writes the preferences and emits a signal so
/// the caller can react.
class SharingDialog : public QDialog {
    Q_OBJECT
public:
    explicit SharingDialog(QWidget* parent = nullptr);

    /// Top-level toggle. Default is **true** (opt-out): the user has to
    /// uncheck this to disable sharing. Per the design discussion,
    /// disabling also stops them from receiving improvements from peers.
    static bool feedbackSharingEnabled();
    static void setFeedbackSharingEnabled(bool on);

signals:
    /// Fires after Accept when the user changed the feedback toggle.
    void feedbackSharingChanged(bool on);

private slots:
    void onAccepted();
    void onResetTrust();

private:
    QCheckBox* m_feedbackBox = nullptr;
    QLabel*    m_summary     = nullptr;
    bool       m_initialFeedback = true;
};

} // namespace censorcut
