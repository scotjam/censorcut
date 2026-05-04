#include "Marker.h"

#include <nlohmann/json.hpp>

namespace censorcut {

QString sourceToString(Source s)
{
    switch (s) {
        case Source::Manual:    return QStringLiteral("manual");
        case Source::Suggested: return QStringLiteral("suggested");
        case Source::Imported:  return QStringLiteral("imported");
    }
    return QStringLiteral("manual");
}

Source sourceFromString(QStringView s)
{
    if (s == u"suggested") return Source::Suggested;
    if (s == u"imported")  return Source::Imported;
    return Source::Manual;
}

QString statusToString(Status s)
{
    switch (s) {
        case Status::Pending:   return QStringLiteral("pending");
        case Status::Confirmed: return QStringLiteral("confirmed");
        case Status::Rejected:  return QStringLiteral("rejected");
    }
    return QStringLiteral("confirmed");
}

Status statusFromString(QStringView s)
{
    if (s == u"pending")  return Status::Pending;
    if (s == u"rejected") return Status::Rejected;
    return Status::Confirmed;
}

void to_json(nlohmann::json& j, const Marker& m)
{
    j = nlohmann::json{
        {"id",         m.id.toString(QUuid::WithoutBraces).toStdString()},
        {"startMs",    m.startMs},
        {"endMs",      m.endMs},
        {"category",   m.category.toStdString()},
        {"note",       m.note.toStdString()},
        {"source",     sourceToString(m.source).toStdString()},
        {"confidence", m.confidence},
        {"status",     statusToString(m.status).toStdString()},
    };
}

void from_json(const nlohmann::json& j, Marker& m)
{
    if (j.contains("id")) {
        m.id = QUuid::fromString(QString::fromStdString(j.at("id").get<std::string>()));
        if (m.id.isNull()) m.id = QUuid::createUuid();
    } else {
        m.id = QUuid::createUuid();
    }
    m.startMs    = j.value("startMs", qint64{0});
    m.endMs      = j.value("endMs",   qint64{0});
    m.category   = QString::fromStdString(j.value("category", std::string{"Manual"}));
    m.note       = QString::fromStdString(j.value("note",     std::string{}));
    m.source     = sourceFromString(QString::fromStdString(
                       j.value("source", std::string{"manual"})));
    m.confidence = j.value("confidence", 1.0);
    m.status     = statusFromString(QString::fromStdString(
                       j.value("status", std::string{"confirmed"})));
}

} // namespace censorcut
