#pragma once

#include <QDialog>
#include <QHash>
#include <QSet>
#include <QString>

class QLabel;
class QTableWidget;
class QPushButton;

namespace censorcut {

/// Reads ~/.censorcut/proposed.jsonl (the file the censorcut-sync sidecar
/// writes when a peer publishes feedback for a category not yet on this
/// machine's accepted list). For each unique category-name plus author
/// signature, the user can:
///
///   - **Accept**: appends the name to ~/.censorcut/accepted_categories.txt
///     so the next analyzer run treats it as a real category. The sidecar
///     reads this file via --accepted-categories-file.
///   - **Dismiss**: writes the name into QSettings under
///     "sharing/dismissedProposedCategories" so this dialog hides it next
///     time. (The sidecar will still receive any further proposals from
///     peers, since dismissal is a UI-only filter.)
class ProposedCategoriesDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProposedCategoriesDialog(QWidget* parent = nullptr);

private slots:
    void onAccept();
    void onDismiss();
    void refresh();

private:
    struct Row {
        QString name;
        int     count = 0;
        QSet<QString> distinctAuthors;
    };
    QHash<QString, Row> readProposals() const;

    QLabel*       m_status = nullptr;
    QTableWidget* m_table  = nullptr;
    QPushButton*  m_acceptBtn  = nullptr;
    QPushButton*  m_dismissBtn = nullptr;
    QPushButton*  m_refreshBtn = nullptr;
};

} // namespace censorcut
