#pragma once

#include "Marker.h"

#include <QAbstractListModel>
#include <QList>

namespace censorcut {

/// List model wrapping a vector of Markers, sorted by startMs.
/// Emits dataChanged/begin/end* properly for use with both QListView and a
/// custom timeline widget.
class MarkerModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        StartMsRole,
        EndMsRole,
        CategoryRole,
        NoteRole,
        SourceRole,
        ConfidenceRole,
        StatusRole,
    };

    explicit MarkerModel(QObject* parent = nullptr);

    // QAbstractListModel
    int      rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool     setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Bulk access (read-only views)
    const QList<Marker>& markers() const { return m_markers; }
    void                 setMarkers(QList<Marker> markers);

    // Mutations — all emit appropriate signals and keep markers sorted.
    void   addMarker(const Marker& m);
    void   removeMarkerAt(int row);
    void   removeMarkerById(const QUuid& id);
    void   updateMarkerById(const QUuid& id, const Marker& replacement);
    void   clear();

    // Lookups
    int                   indexOfId(const QUuid& id) const;
    std::optional<Marker> markerAt(int row) const;
    std::optional<Marker> findById(const QUuid& id) const;

    /// All confirmed markers, sorted by startMs (suitable for export planning).
    QList<Marker> confirmedMarkers() const;

    /// Total cut duration in ms (sum of confirmed marker durations, with overlaps
    /// merged so it's a true coverage number).
    qint64 totalConfirmedCutMs() const;

signals:
    void markerAdded(const QUuid& id);
    void markerRemoved(const QUuid& id);
    void markerChanged(const QUuid& id);

private:
    void resort();

    QList<Marker> m_markers;
};

} // namespace censorcut
