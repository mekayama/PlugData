/*
 // Copyright (c) 2021-2022 Timothy Schoen
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */
#include "Canvas.h"

extern "C"
{
#include <m_pd.h>
#include <m_imp.h>
}

#include "Box.h"
#include "Connection.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LookAndFeel.h"

#include "Utility/GraphArea.h"
#include "Utility/SuggestionComponent.h"

Canvas::Canvas(PlugDataPluginEditor& parent, pd::Patch& p, Component* parentGraph) : main(parent), pd(&parent.pd), patch(p)
{
    isGraphChild = glist_isgraph(p.getPointer());
    hideNameAndArgs = static_cast<bool>(p.getPointer()->gl_hidetext);

    isGraphChild.addListener(this);
    hideNameAndArgs.addListener(this);

    // Check if canvas belongs to a graph
    if (parentGraph)
    {
        setLookAndFeel(&main.getLookAndFeel());
        parentGraph->addAndMakeVisible(this);
        setInterceptsMouseClicks(false, true);
        isGraph = true;
    }
    else
    {
        isGraph = false;
    }

    suggestor = new SuggestionComponent;

    commandLocked.referTo(pd->commandLocked);
    commandLocked.addListener(this);

    gridEnabled.referTo(parent.statusbar.gridEnabled);

    locked.referTo(pd->locked);
    locked.addListener(this);

    tabbar = &parent.tabbar;

    // Add draggable border for setting graph position
    if (static_cast<bool>(isGraphChild.getValue()) && !isGraph)
    {
        graphArea = new GraphArea(this);
        addAndMakeVisible(graphArea);
        graphArea->setAlwaysOnTop(true);
    }

    setSize(600, 400);

    // Add lasso component
    addAndMakeVisible(&lasso);
    lasso.setAlwaysOnTop(true);

    setWantsKeyboardFocus(true);

    if (!isGraph)
    {
        viewport = new Viewport;  // Owned by the tabbar, but doesn't exist for graph!
        viewport->setViewedComponent(this, false);

        // Apply zooming
        setTransform(parent.transform);
        presentationMode.referTo(parent.statusbar.presentationMode);
        presentationMode.addListener(this);
    }
    else
    {
        presentationMode = false;
    }

    synchronise();
}

Canvas::~Canvas()
{
    isBeingDeleted = true;
    delete graphArea;
    delete suggestor;
}


void Canvas::paint(Graphics& g)
{

    if (!isGraph)
    {
        lasso.setColour(LassoComponent<Box>::lassoFillColourId, findColour(PlugDataColour::highlightColourId).withAlpha(0.3f));

        g.fillAll(findColour(PlugDataColour::toolbarColourId));

        g.setColour(findColour(PlugDataColour::canvasColourId));
        g.fillRect(canvasOrigin.x, canvasOrigin.y, getWidth(), getHeight());

        // draw origin
        g.setColour(Colour(100, 100, 100));
        g.drawLine(canvasOrigin.x - 1, canvasOrigin.y - 1, canvasOrigin.x - 1, getHeight() + 2);
        g.drawLine(canvasOrigin.x - 1, canvasOrigin.y - 1, getWidth() + 2, canvasOrigin.y - 1);
    }

    if (locked == var(false) && commandLocked == var(false) && !isGraph)
    {
        const int objectGridSize = 25;
        const Rectangle<int> clipBounds = g.getClipBounds();

        g.setColour(findColour(PlugDataColour::canvasColourId).contrasting(0.42));

        for (int x = canvasOrigin.getX() + objectGridSize; x < clipBounds.getRight(); x += objectGridSize)
        {
            for (int y = canvasOrigin.getY() + objectGridSize; y < clipBounds.getBottom(); y += objectGridSize)
            {
                g.fillRect(x, y, 1, 1);
            }
        }
    }
}

// Synchronise state with pure-data
// Used for loading and for complicated actions like undo/redo
void Canvas::synchronise(bool updatePosition)
{
    pd->waitForStateUpdate();
    deselectAll();

    patch.setCurrent(true);

    auto objects = patch.getObjects();
    auto isObjectDeprecated = [&](void* obj)
    {
        return std::all_of(objects.begin(), objects.end(), [obj](const auto* obj2){
            return obj != obj2;
        });
    };

    if (!(isGraph || presentationMode == var(true)))
    {
        // Remove deprecated connections
        for (int n = connections.size() - 1; n >= 0; n--)
        {
            auto* connection = connections[n];

            if (!connection->inlet || !connection->outlet || isObjectDeprecated(connection->inbox->getPointer()) || isObjectDeprecated(connection->outbox->getPointer()))
            {
                connections.remove(n);
            }
            else
            {
                auto* inlet = static_cast<t_text*>(connection->inbox->getPointer());
                auto* outlet = static_cast<t_text*>(connection->outbox->getPointer());

                if (!canvas_isconnected(patch.getPointer(), outlet, connection->outIdx, inlet, connection->inIdx))
                {
                    connections.remove(n);
                }
            }
        }
    }

    // Clear deleted boxes
    for (int n = boxes.size() - 1; n >= 0; n--)
    {
        auto* box = boxes[n];
        if (box->gui && isObjectDeprecated(box->getPointer()))
        {
            boxes.remove(n);
        }
    }

    for (auto* object : objects)
    {
        auto* it = std::find_if(boxes.begin(), boxes.end(), [&object](Box* b) { return b->getPointer() && b->getPointer() == object; });

        if (it == boxes.end())
        {
            auto* newBox = boxes.add(new Box(object, this));
            newBox->toFront(false);

            // TODO: don't do this on Canvas!!
            if (newBox->gui && newBox->gui->getLabel()) newBox->gui->getLabel()->toFront(false);
        }
        else
        {
            auto* box = *it;

            // Check if number of inlets/outlets is correct
            box->updatePorts();

            // Only update positions if we need to and there is a significant difference
            // There may be rounding errors when scaling the gui, this makes the experience smoother
            if (updatePosition) box->updateBounds();

            box->toFront(false);
            if (box->gui && box->gui->getLabel()) box->gui->getLabel()->toFront(false);
        }
    }

    // Make sure objects have the same order
    std::sort(boxes.begin(), boxes.end(),
              [&objects](Box* first, Box* second) mutable
              {
                  size_t idx1 = std::find(objects.begin(), objects.end(), first->getPointer()) - objects.begin();
                  size_t idx2 = std::find(objects.begin(), objects.end(), second->getPointer()) - objects.begin();

                  return idx1 < idx2;
              });

    auto pdConnections = patch.getConnections();

    if (!(isGraph || presentationMode == var(true)))
    {
        for (auto& connection : pdConnections)
        {
            auto& [inno, inobj, outno, outobj] = connection;

            int srcno = patch.getIndex(&inobj->te_g);
            int sinkno = patch.getIndex(&outobj->te_g);

            auto& srcEdges = boxes[srcno]->edges;
            auto& sinkEdges = boxes[sinkno]->edges;

            // TEMP: remove when we're sure this works
            if (srcno >= boxes.size() || sinkno >= boxes.size() || outno >= srcEdges.size() || inno >= sinkEdges.size())
            {
                pd->logError("Error: impossible connection");
                continue;
            }

            auto* it = std::find_if(connections.begin(), connections.end(),
                                   [this, &connection, &srcno, &sinkno](Connection* c)
                                   {
                                       auto& [inno, inobj, outno, outobj] = connection;

                                       if (!c->inlet || !c->outlet) return false;

                                       bool sameStart = c->outbox == boxes[srcno];
                                       bool sameEnd = c->inbox == boxes[sinkno];

                                       return c->inIdx == inno && c->outIdx == outno && sameStart && sameEnd;
                                   });

            if (it == connections.end())
            {
                connections.add(new Connection(this, srcEdges[boxes[srcno]->numInputs + outno], sinkEdges[inno], true));
            }
            else
            {
                // Update storage ids for connections
                auto& c = *(*it);

                auto currentId = c.getId();
                auto info = pd::Storage::getInfo(this, currentId, "Path");
                if (info.length()) c.setState(info);

                c.repaint();
            }
        }

        setTransform(main.transform);
    }

    // Resize canvas to fit objects
    // By checking asynchronously, we make sure the boxes bounds have been updated
    MessageManager::callAsync([_this = SafePointer(this)](){
        if(!_this) return;
        _this->pd->waitForStateUpdate();
        _this->checkBounds();
    });
    

    main.updateCommandStatus();
    repaint();
}

void Canvas::mouseDown(const MouseEvent& e)
{
    auto* source = e.originalComponent;
    
    PopupMenu::dismissAllActiveMenus();
    
    // Middle mouse click
    if(viewport && ModifierKeys::getCurrentModifiers().isMiddleButtonDown()) {
        setMouseCursor(MouseCursor::UpDownLeftRightResizeCursor);
        viewportPositionBeforeMiddleDrag = viewport->getViewPosition();
    }
    // Left-click
    else if (!ModifierKeys::getCurrentModifiers().isRightButtonDown())
    {
        // Connecting objects by dragging
        if (source == this || source == graphArea)
        {
            if (!connectingEdges.isEmpty())
            {
                connectingEdges.clear();
                repaint();
            }

            lasso.beginLasso(e.getEventRelativeTo(this), this);
            isDraggingLasso = true;
            
            if (!ModifierKeys::getCurrentModifiers().isShiftDown() && !ModifierKeys::getCurrentModifiers().isCommandDown())
            {
                deselectAll();
            }
        }

        // Update selected object in sidebar when we click a box
        if (dynamic_cast<Box*>(source) || source->findParentComponentOfClass<Box>())
        {
            updateSidebarSelection();
        }
        
        main.updateCommandStatus();
    }
    // Right click
    else
    {
        // Info about selection status
        auto& lassoSelection = getLassoSelection();

        auto selectedBoxes = getSelectionOfType<Box>();

        bool hasSelection = !selectedBoxes.isEmpty();
        bool multiple = selectedBoxes.size() > 1;

        Box* box = nullptr;
        if (hasSelection && !multiple) box = selectedBoxes.getFirst();
        
        Array<Box*> parents;
        for (auto* p = source->getParentComponent(); p != nullptr; p = p->getParentComponent()) {
            if (auto* target = dynamic_cast<Box*> (p)) parents.add(target);
        }
        
        // Get top-level parent box... A bit clumsy but otherwise it will open subpatchers deeper down the chain
        if (parents.size() && !hasSelection)  {
            box = parents.getLast();
            hasSelection = true;
        }
        
        bool isSubpatch = box && box->gui && box->gui->getPatch();

        // Create popup menu
        popupMenu.clear();

        popupMenu.addItem(1, "Open", hasSelection && !multiple && isSubpatch);  // for opening subpatches
        popupMenu.addSeparator();

        popupMenu.addCommandItem(&main, CommandIDs::Cut);
        popupMenu.addCommandItem(&main, CommandIDs::Copy);
        popupMenu.addCommandItem(&main, CommandIDs::Paste);
        popupMenu.addCommandItem(&main, CommandIDs::Duplicate);
        popupMenu.addCommandItem(&main, CommandIDs::Delete);
        popupMenu.addSeparator();

        popupMenu.addItem(8, "To Front", box != nullptr);
        popupMenu.addSeparator();
        popupMenu.addItem(9, "Help", box != nullptr);
        popupMenu.addSeparator();
        popupMenu.addItem(10, "Properties", e.originalComponent == this);

        auto callback = [this, box](int result)
        {
            popupMenu.clear();
            if (result < 1) return;
            switch (result)
            {
                case 1:  // Open subpatch
                    box->openSubpatch();
                    break;
                case 8:  // To Front
                    box->toFront(false);
                    if (box->gui) box->gui->moveToFront();
                    break;
                case 9:  // Open help
                    box->openHelpPatch();
                    break;
                case 10:  // Open help
                    main.sidebar.showParameters(parameters);
                    break;
                default:
                    break;
            }
        };

        popupMenu.showMenuAsync(PopupMenu::Options().withMinimumWidth(100).withMaximumNumColumns(1).withParentComponent(&main).withTargetScreenArea(Rectangle<int>(e.getScreenX(), e.getScreenY(), 2, 2)), ModalCallbackFunction::create(callback));
    }
}

void Canvas::mouseDrag(const MouseEvent& e)
{
    // Ignore on graphs or when locked
    if (isGraph || locked == var(true) || commandLocked == var(true))  {
        bool hasToggled = false;
        
        // Behaviour for dragging over toggles, bang and radiogroup to toggle them
        for(auto* box : boxes) {
            if(!box->getBounds().contains(e.getEventRelativeTo(this).getPosition()) || !box->gui) continue;
        
            if(auto* obj = dynamic_cast<GUIObject*>(box->gui.get())) {
                obj->toggleObject(e.getEventRelativeTo(obj).getPosition());
                hasToggled = true;
                break;
            }
        }
        
        if(!hasToggled) {
            for(auto* box : boxes) {
                if(auto* obj = dynamic_cast<GUIObject*>(box->gui.get())) {
                    obj->untoggleObject();
                }
            }
        }

        return;
    }
    
    auto viewportEvent = e.getEventRelativeTo(viewport);

    float scrollSpeed = 8.5f;

    // Middle mouse pan
    if (ModifierKeys::getCurrentModifiers().isMiddleButtonDown())
    {
        auto delta = Point<int>{viewportEvent.getDistanceFromDragStartX(), viewportEvent.getDistanceFromDragStartY()};
        
        viewport->setViewPosition(viewportPositionBeforeMiddleDrag.x - delta.x, viewportPositionBeforeMiddleDrag.y - delta.y);

        return;  // Middle mouse button cancels any other drag actions
    }

    // For fixing coords when zooming
    float scale = (1.0f / static_cast<float>(pd->zoomScale.getValue()));

    
    // Auto scroll when dragging close to the edge
    if (viewport->autoScroll(viewportEvent.x * scale, viewportEvent.y * scale, 50, scrollSpeed))
    {
        beginDragAutoRepeat(40);
    }

    // Drag lasso
    lasso.dragLasso(e);

    if (connectingWithDrag && !connectingEdges.isEmpty())
    {
        auto& connectingEdge = connectingEdges.getReference(0);
        
        auto* nearest = Edge::findNearestEdge(this, e.getEventRelativeTo(this).getPosition(), !connectingEdge->isInlet, connectingEdge->box);

        if (connectingWithDrag && nearest && nearestEdge != nearest)
        {
            nearest->isTargeted = true;

            if (nearestEdge)
            {
                nearestEdge->isTargeted = false;
                nearestEdge->repaint();
            }

            nearestEdge = nearest;
            nearestEdge->repaint();
        }

        repaint();
    }
}

void Canvas::mouseUp(const MouseEvent& e)
{
    setMouseCursor(MouseCursor::NormalCursor);
    main.updateCommandStatus();
    if (auto* box = dynamic_cast<Box*>(e.originalComponent))
    {
        
        if (!popupMenu.getNumItems() && !ModifierKeys::getCurrentModifiers().isShiftDown() && !ModifierKeys::getCurrentModifiers().isCommandDown() && e.getDistanceFromDragStart() < 2)
        {
            deselectAll();
        }

        if (locked == var(false) && !isGraph && box->getParentComponent() == this)
        {
            setSelected(box, true);
        }
    }
    
    
    // Releasing a connect-by-drag action
    if (connectingWithDrag && !connectingEdges.isEmpty() && nearestEdge)
    {
        nearestEdge->isTargeted = false;
        nearestEdge->repaint();
        
        for(auto& edge : connectingEdges) {
            nearestEdge->createConnection();
        }

        if(!e.mods.isShiftDown() || connectingEdges.size() != 1) {
            connectingEdges.clear();
        }
        
        nearestEdge = nullptr;
        connectingWithDrag = false;
        repaint();
    }
    else if (connectingWithDrag)
    {
        connectingEdges.clear();
        connectingWithDrag = false;
        repaint();
    }

    updateSidebarSelection();

    main.updateCommandStatus();

    lasso.endLasso();
    isDraggingLasso = false;
}

void Canvas::updateSidebarSelection()
{
    auto lassoSelection = getSelectionOfType<Box>();

    if (lassoSelection.size() == 1)
    {
        auto* box = lassoSelection.getFirst();
        auto params = box->gui ? box->gui->getParameters() : ObjectParameters();

        if (commandLocked == var(true))
        {
            main.sidebar.hideParameters();
        }
        else if (!params.empty() || main.sidebar.isPinned())
        {
            main.sidebar.showParameters(params);
        }
        else
        {
            main.sidebar.hideParameters();
        }
    }
    else
    {
        main.sidebar.hideParameters();
    }
}

void Canvas::paintOverChildren(Graphics& g)
{
    Point<float> mousePos = getMouseXYRelative().toFloat();
    
    // Draw connections in the making over everything else
    for(auto& edge : connectingEdges)
    {
        Point<int> edgePos = edge->getCanvasBounds().getCentre();

        Path path;
        path.startNewSubPath(edgePos.toFloat());
        path.lineTo(mousePos);

        g.setColour(Colours::grey);
        g.strokePath(path, PathStrokeType(3.0f));
    }
}

void Canvas::mouseMove(const MouseEvent& e)
{
    if (!connectingEdges.isEmpty())
    {
        repaint();
    }
}

bool Canvas::keyPressed(const KeyPress& key)
{
    if (main.getCurrentCanvas() != this || isGraph) return false;

    int keycode = key.getKeyCode();

    auto moveSelection = [this](int x, int y)
    {
        auto boxes = getSelectionOfType<Box>();
        std::vector<void*> objects;

        for (auto* box : boxes)
        {
            objects.push_back(box->getPointer());
        }

        patch.moveObjects(objects, x, y);

        for (auto* box : boxes)
        {
            box->updateBounds();

            if (!getBounds().contains(box->getBounds()))
            {
                checkBounds();
            }
        }
    };

    // Move objects with arrow keys
    if (keycode == KeyPress::leftKey)
    {
        moveSelection(-10, 0);
        return true;
    }
    if (keycode == KeyPress::rightKey)
    {
        moveSelection(10, 0);
        return true;
    }
    if (keycode == KeyPress::upKey)
    {
        moveSelection(0, -10);
        return true;
    }
    if (keycode == KeyPress::downKey)
    {
        moveSelection(0, 10);
        return true;
    }

    // Ignore backspace, arrow keys, return key and more that might cause actions in pd
    if (keycode == KeyPress::backspaceKey || keycode == KeyPress::pageUpKey || keycode == KeyPress::pageDownKey || keycode == KeyPress::homeKey || keycode == KeyPress::escapeKey || keycode == KeyPress::deleteKey || keycode == KeyPress::returnKey || keycode == KeyPress::tabKey)
    {
        return false;
    }

    patch.keyPress(keycode, key.getModifiers().isShiftDown());

    return false;
}

void Canvas::deselectAll()
{
    // Deselect boxes
    for (auto c : selectedComponents)
        if (!c.wasObjectDeleted()) c->repaint();

    selectedComponents.deselectAll();
    main.sidebar.hideParameters();
}

void Canvas::copySelection()
{
    // Tell pd to select all objects that are currently selected
    for (auto sel : getLassoSelection())
    {
        if (auto* box = dynamic_cast<Box*>(sel.get()))
        {
            patch.selectObject(box->getPointer());
        }
    }

    // Tell pd to copy
    patch.copy();
    patch.deselectAll();
}

void Canvas::pasteSelection()
{
    // Tell pd to paste
    patch.paste();

    // Load state from pd, don't update positions
    synchronise(false);

    for (auto* box : boxes)
    {
        if (glist_isselected(patch.getPointer(), static_cast<t_gobj*>(box->getPointer())))
        {
            setSelected(box, true);
        }
    }

    patch.deselectAll();
}

void Canvas::duplicateSelection()
{
    // Tell pd to select all objects that are currently selected
    for (auto sel : getLassoSelection())
    {
        if (auto* box = dynamic_cast<Box*>(sel.get()))
        {
            patch.selectObject(box->getPointer());
        }
    }

    // Tell pd to duplicate
    patch.duplicate();

    // Load state from pd, don't update positions
    synchronise(false);

    // Select the newly duplicated objects
    for (auto* box : boxes)
    {
        if (glist_isselected(patch.getPointer(), static_cast<t_gobj*>(box->getPointer())))
        {
            setSelected(box, true);
        }
    }

    patch.deselectAll();
}

void Canvas::removeSelection()
{
    // Make sure object isn't selected and stop updating gui
    main.sidebar.hideParameters();

    // Make sure nothing is selected
    patch.deselectAll();

    // Find selected objects and make them selected in pd
    Array<void*> objects;
    for (auto sel : getLassoSelection())
    {
        if (auto* box = dynamic_cast<Box*>(sel.get()))
        {
            if (box->getPointer())
            {
                patch.selectObject(box->getPointer());
                objects.add(box->getPointer());
            }
        }
    }

    // remove selection
    patch.removeSelection();

    // Remove connection afterwards and make sure they aren't already deleted
    for (auto* con : connections)
    {
        if (isSelected(con))
        {
            if (!(objects.contains(con->outbox->getPointer()) || objects.contains(con->inbox->getPointer())))
            {
                patch.removeConnection(con->outbox->getPointer(), con->outIdx, con->inbox->getPointer(), con->inIdx);
            }
        }
    }

    patch.finishRemove();  // Makes sure that the extra removed connections will be grouped in the same undo action

    deselectAll();

    // Load state from pd, don't update positions
    synchronise(false);

    patch.deselectAll();
}

void Canvas::undo()
{
    // Performs undo on storage data if the next undo event if a dummy
    pd::Storage::undoIfNeeded(this);

    // Tell pd to undo the last action
    patch.undo();

    // Load state from pd
    synchronise();

    patch.deselectAll();
}

void Canvas::redo()
{
    // Performs redo on storage data if the next redo event if a dummy
    pd::Storage::redoIfNeeded(this);

    // Tell pd to undo the last action
    patch.redo();

    // Load state from pd
    synchronise();

    patch.deselectAll();
}

void Canvas::checkBounds()
{
    if (isGraph || !viewport) return;
    
    updatingBounds = true;

    float scale = (1.0f / static_cast<float>(pd->zoomScale.getValue()));

    auto viewBounds = Rectangle<int>(canvasOrigin.x, canvasOrigin.y, viewport->getMaximumVisibleWidth() * scale, viewport->getMaximumVisibleHeight() * scale);

    for (auto* obj : boxes)
    {
        viewBounds = obj->getBounds().getUnion(viewBounds);
    }

    canvasOrigin -= {viewBounds.getX(), viewBounds.getY()};
    setSize(viewBounds.getWidth(), viewBounds.getHeight());

    for (auto& box : boxes)
    {
        box->updateBounds();
    }

    if (graphArea)
    {
        graphArea->updateBounds();
    }

    updatingBounds = false;
}

void Canvas::valueChanged(Value& v)
{
    // When lock changes
    if (v.refersToSameSourceAs(locked))
    {
        if (!connectingEdges.isEmpty()) connectingEdges.clear();
        deselectAll();
        repaint();
        
        // Makes sure no objects keep keyboard focus after locking/unlocking
        if(isShowing() && isVisible()) grabKeyboardFocus();
    }
    else if (v.refersToSameSourceAs(commandLocked))
    {
        repaint();
    }
    // Should only get called when the canvas isn't a real graph
    else if (v.refersToSameSourceAs(presentationMode))
    {
        deselectAll();

        if (presentationMode == var(true)) connections.clear();

        commandLocked.setValue(presentationMode.getValue());

        synchronise();
    }
    else if (v.refersToSameSourceAs(isGraphChild))
    {
        patch.getPointer()->gl_isgraph = static_cast<bool>(isGraphChild.getValue());

        if (static_cast<bool>(isGraphChild.getValue()) && !isGraph)
        {
            graphArea = new GraphArea(this);
            addAndMakeVisible(graphArea);
            graphArea->setAlwaysOnTop(true);
            graphArea->updateBounds();
        }
        else
        {
            delete graphArea;
            graphArea = nullptr;
        }
        repaint();
    }
    else if (v.refersToSameSourceAs(hideNameAndArgs))
    {
        patch.getPointer()->gl_hidetext = static_cast<bool>(hideNameAndArgs.getValue());
        repaint();
    }
}

void Canvas::showSuggestions(Box* box, TextEditor* editor)
{
    suggestor->createCalloutBox(box, editor);
}
void Canvas::hideSuggestions()
{
    suggestor->removeCalloutBox();
}

// Makes component selected
void Canvas::setSelected(Component* component, bool shouldNowBeSelected)
{
    bool isAlreadySelected = isSelected(component);

    if (!isAlreadySelected && shouldNowBeSelected)
    {
        selectedComponents.addToSelection(component);
        component->repaint();
    }

    if (isAlreadySelected && !shouldNowBeSelected)
    {
        removeSelectedComponent(component);
        component->repaint();
    }

    main.updateCommandStatus();
}

bool Canvas::isSelected(Component* component) const
{
    return std::find(selectedComponents.begin(), selectedComponents.end(), component) != selectedComponents.end();
}

void Canvas::handleMouseDown(Component* component, const MouseEvent& e)
{
    if (!isSelected(component))
    {
        if (!(e.mods.isShiftDown() || e.mods.isCommandDown()))  {
            
            // Deselect boxes and connections
            for (auto c : selectedComponents) {
                if (!c.wasObjectDeleted() || c.get() != this)  {
                    setSelected(c.get(), false);
                    c->repaint();
                }
            }
        }

        setSelected(component, true);
    }

    if (auto* box = dynamic_cast<Box*>(component))
    {
        componentBeingDragged = box;
    }

    for (auto* box : getSelectionOfType<Box>())
    {
        box->mouseDownPos = box->getPosition();
    }

    if (component)
    {
        component->repaint();
    }
    
    canvasDragStartPosition = getPosition();
}

// Call from component's mouseUp
void Canvas::handleMouseUp(Component* component, const MouseEvent& e)
{
    if (didStartDragging)
    {
        auto objects = std::vector<void*>();

        for (auto component : getLassoSelection())
        {
            if (auto* box = dynamic_cast<Box*>(component.get()))
            {
                if (box->getPointer()) objects.push_back(box->getPointer());
            }
        }

        auto distance = Point<int>(e.getDistanceFromDragStartX(), e.getDistanceFromDragStartY());

        // In case we dragged near the edge and the canvas moved
        auto canvasMoveOffset = canvasDragStartPosition - getPosition();
        
        distance = grid.handleMouseUp(distance) + canvasMoveOffset;

        // When done dragging objects, update positions to pd
        patch.moveObjects(objects, distance.x, distance.y);

        pd->waitForStateUpdate();
        
        // Update undo state
        main.updateCommandStatus();
        
        checkBounds();
        didStartDragging = false;
    }
    
    if(boxSnappingInbetween) {
        auto* c = connectionToSnapInbetween.getComponent();
        
        patch.removeConnection(c->outbox->getPointer(), c->outIdx, c->inbox->getPointer(), c->inIdx);
        
        patch.createConnection(c->outbox->getPointer(), c->outIdx, boxSnappingInbetween->getPointer(), 0);
        patch.createConnection(boxSnappingInbetween->getPointer(), 0, c->inbox->getPointer(), c->inIdx);
        
        boxSnappingInbetween->edges[0]->isTargeted = false;
        boxSnappingInbetween->edges[boxSnappingInbetween->numInputs]->isTargeted = false;
        boxSnappingInbetween = nullptr;
        
        synchronise();
    }

    componentBeingDragged = nullptr;

    component->repaint();
}

// Call from component's mouseDrag
void Canvas::handleMouseDrag(const MouseEvent& e)
{
    /** Ensure tiny movements don't start a drag. */
    if (!didStartDragging && e.getDistanceFromDragStart() < minimumMovementToStartDrag) return;

    if(!didStartDragging)  {
        didStartDragging = true;
        main.updateCommandStatus();
    }

    auto dragDistance = e.getOffsetFromDragStart();

    if (static_cast<bool>(gridEnabled.getValue()) && componentBeingDragged)
    {
        dragDistance = grid.handleMouseDrag(componentBeingDragged, dragDistance, viewport->getViewArea());
    }
    
    auto selection = getSelectionOfType<Box>();

    for (auto* box : selection)
    {
        // In case we dragged near the edge and the canvas moved
        auto canvasMoveOffset = canvasDragStartPosition - getPosition();
        box->setTopLeftPosition(box->mouseDownPos + dragDistance + canvasMoveOffset);
    }
    
    // Behaviour for shift-dragging objects over
    if(boxSnappingInbetween) {
        bool stillSnapped = false;
        if(connectionToSnapInbetween->intersectsObject(boxSnappingInbetween)) {
            stillSnapped = true;
            return;
        }
        
        // If we're here, it's not snapping anymore
        boxSnappingInbetween->edges[0]->isTargeted = false;
        boxSnappingInbetween->edges[boxSnappingInbetween->numInputs]->isTargeted = false;
        boxSnappingInbetween = nullptr;
    }
    
    if(e.mods.isShiftDown() && selection.size() == 1) {
        auto* box = selection.getFirst();
        if(box->numInputs >= 1 && box->numOutputs >= 1) {
            
            /*
            std::vector<Connection*> start;
            std::vector<Connection*> end;
            for(auto* connection : connections) {
                if(connection->outlet == box->edges[box->numOutputs]) start.push_back(connection);
                if(connection->inlet == box->edges[0])                end.push_back(connection);
            }
            
            // Don't handle ambiguous cases
            if(start.size() == 1 && end.size() == 1)
            {
                patch.removeConnection(start->outbox->getPointer(), start->outIdx, start->inbox->getPointer(), start->inIdx);
                patch.removeConnection(end->outbox->getPointer(), end->outIdx, end->inbox->getPointer(), end->inIdx);
                patch.createConnection(start->outbox->getPointer(), start->outIdx, end->outbox->getPointer(), end->outIdx);
                synchronise();
            } */
            
            for(auto* connection : connections) {
                if(connection->intersectsObject(box)) {
                    box->edges[0]->isTargeted = true;
                    box->edges[box->numInputs]->isTargeted = true;
                    connectionToSnapInbetween = connection;
                    boxSnappingInbetween = box;
                    break;
                }
            }
            
            
        }
        
        
    }
}

SelectedItemSet<WeakReference<Component>>& Canvas::getLassoSelection()
{
    return selectedComponents;
}

void Canvas::removeSelectedComponent(Component* component)
{
    selectedComponents.deselect(component);
}

void Canvas::findLassoItemsInArea(Array<WeakReference<Component>>& itemsFound, const Rectangle<int>& area)
{
    for (auto* element : boxes)
    {
        if (area.intersects(element->getBounds()))
        {
            itemsFound.add(element);
            setSelected(element, true);
            element->repaint();
        }
        else if (!ModifierKeys::getCurrentModifiers().isAnyModifierKeyDown())
        {
            setSelected(element, false);
        }
    }

    for (auto& con : connections)
    {
        // If total bounds don't intersect, there can't be an intersection with the line
        // This is cheaper than checking the path intersection, so do this first
        if (!con->getBounds().intersects(lasso.getBounds())) continue;

        // Check if path intersects with lasso
        if (con->intersects(lasso.getBounds().translated(-con->getX(), -con->getY()).toFloat()))
        {
            itemsFound.add(con);
            setSelected(con, true);
        }
        else if (!ModifierKeys::getCurrentModifiers().isAnyModifierKeyDown())
        {
            setSelected(con, false);
        }
    }
}
