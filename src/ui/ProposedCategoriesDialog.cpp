#include "ProposedCategoriesDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

namespace censorcut {

namespace {

constexpr const char* kDismissedKey = "sharing/dismissedProposedCategories";

QString proposedPath()
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return QDir::cleanPath(home + QStringLiteral("/.censorcut/proposed.jsonl"));
}

QString acceptedListPath()
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return QDir::cleanPath(home + QStringLiteral("/.censorcut/accepted_categories.txt"));
}

bool isAlnumLike(const QString& s)
{
    if (s.isEmpty() || s.size() > 64) return false;
    for (QChar c : s) {
        if (!c.isLetterOrNumber()) return false;
    }
    return true;
}

QSet<QString> dismissedSet()
{
    QSettings st;
    const QStringList list = st.value(QLatin1String(kDismissedKey)).toStringList();
    return QSet<QString>(list.cbegin(), list.cend());
}

void appendDismissed(const QString& name)
{
    QSettings st;
    QStringList list = st.value(QLatin1String(kDismissedKey)).toStringList();
    if (!list.contains(name)) {
        list.append(name);
        st.setValue(QLatin1String(kDismissedKey), list);
    }
}

QSet<QString> readAlreadyAccepted()
{
    QSet<QString> result;
    QFile f(acceptedListPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return result;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            result.insert(line.toLower());
        }
    }
    return result;
}

bool appendAccepted(const QString& name, QString* err)
{
    const QString path = acceptedListPath();
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) {
        if (err) *err = f.errorString();
        return false;
    }
    QTextStream out(&f);
    out << name.toLower() << "\n";
    return true;
}

} // namespace

ProposedCategoriesDialog::ProposedCategoriesDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Proposed categories from peers"));
    setMinimumSize(620, 360);

    auto* main = new QVBoxLayout(this);
    auto* intro = new QLabel(this);
    intro->setWordWrap(true);
    intro->setText(tr(
        "Peers can suggest new category namespaces (e.g. <i>Bullying</i>, "
        "<i>Drug references</i>) that aren't on your accepted list yet. "
        "Names that look reasonable can be accepted; once accepted, the "
        "next analyzer run treats them as real categories. Dismissed "
        "names are hidden from this dialog but the sidecar will still "
        "receive any further proposals from peers."));
    main->addWidget(intro);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    main->addWidget(m_status);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({
        tr("Category name"), tr("Proposals"), tr("Distinct peers")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    main->addWidget(m_table, /*stretch=*/1);

    auto* btns = new QHBoxLayout;
    m_acceptBtn  = new QPushButton(tr("Accept selected"), this);
    m_dismissBtn = new QPushButton(tr("Dismiss selected"), this);
    m_refreshBtn = new QPushButton(tr("Refresh"), this);
    auto* close  = new QPushButton(tr("Close"), this);
    btns->addWidget(m_acceptBtn);
    btns->addWidget(m_dismissBtn);
    btns->addStretch(1);
    btns->addWidget(m_refreshBtn);
    btns->addWidget(close);
    main->addLayout(btns);

    connect(m_acceptBtn,  &QPushButton::clicked, this, &ProposedCategoriesDialog::onAccept);
    connect(m_dismissBtn, &QPushButton::clicked, this, &ProposedCategoriesDialog::onDismiss);
    connect(m_refreshBtn, &QPushButton::clicked, this, &ProposedCategoriesDialog::refresh);
    connect(close,        &QPushButton::clicked, this, &QDialog::accept);

    refresh();
}

QHash<QString, ProposedCategoriesDialog::Row>
ProposedCategoriesDialog::readProposals() const
{
    QHash<QString, Row> out;
    QFile f(proposedPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        try {
            const auto j = nlohmann::json::parse(line.toStdString());
            const QString name = QString::fromStdString(j.value("category", std::string()));
            const QString author = QString::fromStdString(j.value("author", std::string()));
            if (!isAlnumLike(name)) continue;
            const QString key = name.toLower();
            Row& r = out[key];
            r.name = name.toLower();
            ++r.count;
            if (!author.isEmpty()) r.distinctAuthors.insert(author);
        } catch (...) {
            // skip malformed line
        }
    }
    return out;
}

void ProposedCategoriesDialog::refresh()
{
    const QHash<QString, Row> all = readProposals();
    const QSet<QString> dismissed = dismissedSet();
    const QSet<QString> already   = readAlreadyAccepted();

    m_table->setRowCount(0);
    int hidden = 0;
    QList<Row> rows;
    rows.reserve(all.size());
    for (auto it = all.cbegin(); it != all.cend(); ++it) {
        if (dismissed.contains(it.key()) || already.contains(it.key())) {
            ++hidden;
            continue;
        }
        rows.append(it.value());
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.name < b.name;
    });
    for (const Row& r : rows) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(r.name));
        m_table->setItem(row, 1, new QTableWidgetItem(QString::number(r.count)));
        m_table->setItem(row, 2, new QTableWidgetItem(QString::number(r.distinctAuthors.size())));
    }
    m_status->setText(tr("%1 actionable, %2 already accepted/dismissed/hidden.")
                          .arg(rows.size()).arg(hidden));
}

void ProposedCategoriesDialog::onAccept()
{
    const int row = m_table->currentRow();
    if (row < 0) return;
    const QString name = m_table->item(row, 0)->text();
    QString err;
    if (!appendAccepted(name, &err)) {
        QMessageBox::warning(this, tr("Could not accept"),
            tr("Could not write to %1: %2")
                .arg(acceptedListPath()).arg(err));
        return;
    }
    refresh();
}

void ProposedCategoriesDialog::onDismiss()
{
    const int row = m_table->currentRow();
    if (row < 0) return;
    appendDismissed(m_table->item(row, 0)->text());
    refresh();
}

} // namespace censorcut
