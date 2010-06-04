/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2010 Robert Bieber
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/


#include "parsetreemodel.h"
#include "symbols.h"

#include <cstdlib>

#include <QObject>

ParseTreeModel::ParseTreeModel(const char* document, QObject* parent):
        QAbstractItemModel(parent)
{
    this->tree = skin_parse(document);

    if(tree)
        this->root = new ParseTreeNode(tree);
    else
        this->root = 0;
}


ParseTreeModel::~ParseTreeModel()
{
    if(root)
        delete root;
    if(tree)
        skin_free_tree(tree);
}

QString ParseTreeModel::genCode()
{
    return root->genCode();
}

bool ParseTreeModel::changeTree(const char *document)
{
    struct skin_element* test = skin_parse(document);

    if(!test)
        return false;

    ParseTreeNode* temp = new ParseTreeNode(test);
    if(root && temp->genHash() == root->genHash())
    {
        delete temp;
        return true;
    }

    if(root)
    {
        emit beginRemoveRows(QModelIndex(), 0, root->numChildren() - 1);
        delete root;
        emit endRemoveRows();
    }

    root = temp;

    emit beginInsertRows(QModelIndex(), 0, temp->numChildren() - 1);
    emit endInsertRows();

    return true;

}

QModelIndex ParseTreeModel::index(int row, int column,
                                  const QModelIndex& parent) const
{
    if(!hasIndex(row, column, parent))
        return QModelIndex();

    ParseTreeNode* foundParent;

    if(parent.isValid())
        foundParent = static_cast<ParseTreeNode*>(parent.internalPointer());
    else
        foundParent = root;

    if(row < foundParent->numChildren() && row >= 0)
        return createIndex(row, column, foundParent->child(row));
    else
        return QModelIndex();
}

QModelIndex ParseTreeModel::parent(const QModelIndex &child) const
{
    if(!child.isValid())
        return QModelIndex();

    ParseTreeNode* foundParent = static_cast<ParseTreeNode*>
                                 (child.internalPointer())->getParent();

    if(foundParent == root)
        return QModelIndex();

    return createIndex(foundParent->getRow(), 0, foundParent);
}

int ParseTreeModel::rowCount(const QModelIndex &parent) const
{
    if(!root)
        return 0;

    if(!parent.isValid())
        return root->numChildren();

    if(parent.column() != typeColumn)
        return 0;

    return static_cast<ParseTreeNode*>(parent.internalPointer())->numChildren();
}

int ParseTreeModel::columnCount(const QModelIndex &parent) const
{
    if(parent.isValid())
        return numColumns;
    else
        return numColumns;
}

QVariant ParseTreeModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid())
        return QVariant();

    if(role != Qt::DisplayRole)
        return QVariant();

    return static_cast<ParseTreeNode*>(index.internalPointer())->
            data(index.column());
}

QVariant ParseTreeModel::headerData(int col, Qt::Orientation orientation,
                                    int role) const
{
    if(orientation != Qt::Horizontal)
        return QVariant();

    if(col >= numColumns)
        return QVariant();

    if(role != Qt::DisplayRole)
        return QVariant();

    switch(col)
    {
    case typeColumn:
        return QObject::tr("Type");

    case lineColumn:
        return QObject::tr("Line");

    case valueColumn:
        return QObject::tr("Value");
    }

    return QVariant();
}

Qt::ItemFlags ParseTreeModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags retval = Qt::ItemIsEnabled | Qt::ItemIsSelectable;

    ParseTreeNode* element = static_cast<ParseTreeNode*>
                             (index.internalPointer());

    if((element->isParam()
        || element->getElement()->type == TEXT
        || element->getElement()->type == COMMENT)
        && index.column() == valueColumn)
    {
        retval |= Qt::ItemIsEditable;
    }

    return retval;
}

bool ParseTreeModel::setData(const QModelIndex &index, const QVariant &value,
                             int role)
{
    if(role != Qt::EditRole)
        return false;

    if(index.column() != valueColumn)
        return false;

    ParseTreeNode* node = static_cast<ParseTreeNode*>
                             (index.internalPointer());

    if(node->isParam())
    {
        struct skin_tag_parameter* param = node->getParam();

        /* Now that we've established that we do, in fact, have a parameter,
         * set it to its new value if an acceptable one has been entered
         */
        if(value.toString().trimmed() == QString(QChar(DEFAULTSYM)))
        {
            if(islower(param->type_code))
                param->type = skin_tag_parameter::DEFAULT;
            else
                return false;
        }
        else if(tolower(param->type_code) == 's'
                || tolower(param->type_code) == 'f')
        {
            if(param->type == skin_tag_parameter::STRING)
                free(param->data.text);

            param->type = skin_tag_parameter::STRING;
            param->data.text = strdup(value.toString().trimmed().toAscii());
        }
        else if(tolower(param->type_code) == 'i')
        {
            if(!value.canConvert(QVariant::Int))
                return false;

            param->type = skin_tag_parameter::NUMERIC;
            param->data.numeric = value.toInt();
        }
        else
        {
            return false;
        }
    }
    else
    {
        struct skin_element* element = node->getElement();

        if(element->type != COMMENT && element->type != TEXT)
            return false;

        free(element->text);
        element->text = strdup(value.toString().trimmed().toAscii());
    }

    emit dataChanged(index, index);
    return true;
}