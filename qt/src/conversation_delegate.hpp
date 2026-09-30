#ifndef CHAT_QT_SRC_CONVERSATION_DELEGATE_HPP
#define CHAT_QT_SRC_CONVERSATION_DELEGATE_HPP

#include <QStyledItemDelegate>

class conversation_delegate final : public QStyledItemDelegate
{
    Q_OBJECT

   public:
    explicit conversation_delegate(QObject* parent = nullptr);

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option,
                     QModelIndex const& index) override;

   signals:
    void avatar_clicked(QModelIndex const& index);
};

#endif
