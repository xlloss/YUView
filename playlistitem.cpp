#include "playlistitem.h"

#include "playlistitemvid.h"

PlaylistItem::PlaylistItem(const QString &itemName, QTreeWidget * parent) : QTreeWidgetItem ( parent, 1001 )
{
    p_displayObject = NULL;

    // update item name to short name
    setText(0, itemName);

    // update icon
    setIcon(0, QIcon());

    // enable dragging
    setFlags(flags() | Qt::ItemIsDragEnabled);
}

PlaylistItem::PlaylistItem(const QString &itemName, QTreeWidgetItem* parentItem) : QTreeWidgetItem( parentItem, 1001 )
{
    p_displayObject = NULL;

    // update item name to short name
    setText(0, itemName);

    // update icon
    setIcon(0, QIcon());

    // enable dragging
    setFlags(flags() | Qt::ItemIsDragEnabled);
}

PlaylistItem::~PlaylistItem() {
    if (p_displayObject!=NULL)
    {
        delete p_displayObject;
    }
}
