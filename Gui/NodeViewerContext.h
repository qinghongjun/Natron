/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2016 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef NODEVIEWERCONTEXT_H
#define NODEVIEWERCONTEXT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <QObject>

#include "Gui/GuiFwd.h"
#include "Gui/KnobGuiContainerI.h"

NATRON_NAMESPACE_ENTER;


struct NodeViewerContextPrivate;
class NodeViewerContext
    : public QObject
      , public KnobGuiContainerI
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    NodeViewerContext(const NodeGuiPtr& node, ViewerTab* viewer);

    virtual ~NodeViewerContext();

    /**
     * @brief Return the container widget for the controls of the node on the viewer
     **/
    QWidget* getButtonsContainer() const;

    /**
     * @brief If this node's viewer context has a toolbar, this will return it
     **/
    QToolBar* getToolBar() const;

    /**
     * @brief The selected role ID
     **/
    const QString& getCurrentRole() const;

    /**
     * @brief The selected tool ID
     **/
    const QString& getCurrentTool() const;
    virtual Gui* getGui() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const QUndoCommand* getLastUndoCommand() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void pushUndoCommand(QUndoCommand* cmd) OVERRIDE FINAL;
    virtual KnobGuiPtr getKnobGui(const KnobPtr& knob) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    void setCurrentTool(const QString& toolID, bool notifyNode);

Q_SIGNALS:

    /**
     * @brief Emitted when the selected role changes
     **/
    void roleChanged(int previousRole, int newRole);

public Q_SLOTS:

    void onToolActionTriggered();

    void onToolActionTriggered(QAction* act);

private:

    boost::scoped_ptr<NodeViewerContextPrivate> _imp;
};

NATRON_NAMESPACE_EXIT;

#endif // NODEVIEWERCONTEXT_H
