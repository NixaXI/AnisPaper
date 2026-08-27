#pragma once

#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>

class CatalogModel : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

 public:
  enum Roles {
    IdRole = Qt::UserRole + 1,
    TitleRole,
    TypeRole,
    PreviewRole,
    FavoriteRole
  };

  explicit CatalogModel(QObject *parent = nullptr);

  int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  void setItems(const QJsonArray &items);
  void setFavorites(const QStringList &favorites);
  void setFilter(const QString &filter);

  QJsonObject itemById(const QString &id) const;
  QVariantList allItems() const;

 signals:
  void countChanged();

 private:
  void rebuild();

  QJsonArray all_;
  QJsonArray visible_;
  QSet<QString> favorites_;
  QString filter_;
};
