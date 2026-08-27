#include "catalog_model.h"

#include <QJsonArray>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

CatalogModel::CatalogModel(QObject *parent) : QAbstractListModel(parent) {}

int CatalogModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : visible_.size();
}

QVariant CatalogModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= visible_.size()) {
    return {};
  }
  const auto item = visible_.at(index.row()).toObject();
  switch (role) {
    case IdRole:
      return item.value("id").toString();
    case TitleRole:
      return item.value("title").toString();
    case TypeRole:
      return item.value("type").toString();
    case PreviewRole:
      return item.value("preview").toString();
    case FavoriteRole:
      return favorites_.contains(item.value("id").toString());
    default:
      return {};
  }
}

QHash<int, QByteArray> CatalogModel::roleNames() const {
  return {{IdRole, "itemId"},
          {TitleRole, "title"},
          {TypeRole, "type"},
          {PreviewRole, "preview"},
          {FavoriteRole, "favorite"}};
}

void CatalogModel::setItems(const QJsonArray &items) {
  all_ = items;
  rebuild();
}

void CatalogModel::setFavorites(const QStringList &favorites) {
  favorites_ = QSet<QString>(favorites.begin(), favorites.end());
  if (!visible_.isEmpty()) {
    emit dataChanged(index(0), index(visible_.size() - 1), {FavoriteRole});
  }
}

void CatalogModel::setFilter(const QString &filter) {
  if (filter_ == filter) return;
  filter_ = filter;
  rebuild();
}

QJsonObject CatalogModel::itemById(const QString &id) const {
  for (const auto &value : all_) {
    const auto item = value.toObject();
    if (item.value("id").toString() == id) return item;
  }
  return {};
}

QVariantList CatalogModel::allItems() const {
  QVariantList rows;
  for (const auto &value : all_) {
    const auto item = value.toObject();
    const auto id = item.value("id").toString();
    QVariantMap row;
    row.insert(QStringLiteral("id"), id);
    row.insert(QStringLiteral("title"), item.value("title").toString());
    row.insert(QStringLiteral("type"), item.value("type").toString());
    row.insert(QStringLiteral("preview"), item.value("preview").toString());
    row.insert(QStringLiteral("favorite"), favorites_.contains(id));
    QStringList tags;
    for (const auto &tag : item.value("tags").toArray()) {
      if (tag.isString()) tags << tag.toString();
    }
    row.insert(QStringLiteral("tags"), tags);
    rows.append(row);
  }
  return rows;
}

void CatalogModel::rebuild() {
  QJsonArray preferred;
  QJsonArray rest;
  const auto needle = filter_.trimmed().toLower();
  for (const auto &value : all_) {
    const auto item = value.toObject();
    const auto title = item.value("title").toString();
    const auto id = item.value("id").toString();
    const auto haystack =
        (title + QLatin1Char(' ') + id + QLatin1Char(' ') +
         item.value("type").toString())
            .toLower();
    if (!needle.isEmpty() && !haystack.contains(needle)) continue;
    const auto folded = title.toLower();
    if (favorites_.contains(id) || folded.contains(QLatin1String("starlight")) ||
        folded.contains(QLatin1String("anis"))) {
      preferred.append(item);
    } else {
      rest.append(item);
    }
  }
  QJsonArray next;
  for (const auto &value : preferred) next.append(value);
  for (const auto &value : rest) next.append(value);
  beginResetModel();
  visible_ = next;
  endResetModel();
  emit countChanged();
}
