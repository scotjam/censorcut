#include "SharingDialog.h"

#include "core/TrustLedger.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace censorcut {

namespace {
constexpr const char* kFeedbackKey = "sharing/feedbackEnabled";
}

bool SharingDialog::feedbackSharingEnabled()
{
    QSettings s;
    return s.value(QLatin1String(kFeedbackKey), true).toBool();
}

void SharingDialog::setFeedbackSharingEnabled(bool on)
{
    QSettings s;
    s.setValue(QLatin1String(kFeedbackKey), on);
}

SharingDialog::SharingDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Sharing settings"));
    setMinimumWidth(560);
    m_initialFeedback = feedbackSharingEnabled();

    auto* main = new QVBoxLayout(this);

    auto* intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setText(tr(
        "<p>CensorCut can share your <i>accept/reject decisions</i> "
        "with other users over a peer-to-peer network so everyone's "
        "analyzer gets better over time. Your installation receives "
        "definitions that provide automatic editor improvements based "
        "on other users' feedback. Sharing is two-directional — "
        "turning it off here also stops your installation from "
        "receiving those improvements.</p>"
        "<p>The data shared is the analyzer's semantic vector for "
        "the scene, the category, and your accept/reject choice. "
        "Film titles and file paths are not shared. CensorCut "
        "itself does not log IP addresses.</p>"));
    main->addWidget(intro);

    auto* group = new QGroupBox(tr("Feedback (M7)"), this);
    auto* gv = new QVBoxLayout(group);
    m_feedbackBox = new QCheckBox(tr(
        "Share accept/reject decisions and receive improvements from peers "
        "(opt-out, default ON)"), group);
    m_feedbackBox->setChecked(m_initialFeedback);
    gv->addWidget(m_feedbackBox);
    main->addWidget(group);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setText(tr(
        "<small>Edit packs (M8.5) and category-definition sharing (M8.6) "
        "are <i>opt-in</i> and configured separately under Tools.</small>"));
    main->addWidget(m_summary);

    auto* trustGroup = new QGroupBox(tr("Reputation"), this);
    auto* tv = new QVBoxLayout(trustGroup);
    auto* trustText = new QLabel(trustGroup);
    trustText->setWordWrap(true);
    trustText->setText(tr(
        "<small>This installation maintains a local reputation score "
        "for each peer based on whether their feedback led to "
        "suggestions you accepted. Resetting wipes those scores so "
        "every peer starts fresh.</small>"));
    tv->addWidget(trustText);
    auto* trustRow = new QHBoxLayout;
    auto* resetTrust = new QPushButton(tr("Reset trust scores..."), trustGroup);
    trustRow->addWidget(resetTrust);
    trustRow->addStretch(1);
    tv->addLayout(trustRow);
    main->addWidget(trustGroup);
    connect(resetTrust, &QPushButton::clicked, this, &SharingDialog::onResetTrust);

    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    btns->button(QDialogButtonBox::Save)->setText(tr("Save"));
    main->addWidget(btns);

    connect(btns, &QDialogButtonBox::accepted, this, &SharingDialog::onAccepted);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SharingDialog::onResetTrust()
{
    const auto answer = QMessageBox::warning(
        this, tr("Reset trust scores"),
        tr("This will wipe every peer's reputation score and stored "
           "endorsement data on this installation. Every peer will start "
           "fresh next time their feedback is consulted. Continue?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    TrustLedger ledger;
    ledger.reset();
}

void SharingDialog::onAccepted()
{
    const bool now = m_feedbackBox->isChecked();
    if (now != m_initialFeedback) {
        setFeedbackSharingEnabled(now);
        emit feedbackSharingChanged(now);
    }
    accept();
}

} // namespace censorcut
