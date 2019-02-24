#include "disassemblertextview.h"
#include "../../models/disassemblermodel.h"
#include <redasm/plugins/format.h>
#include <QtWidgets>
#include <QtGui>
#include <cmath>

#define CURSOR_BLINK_INTERVAL 500  // 500ms
#define FALLBACK_REFRESH_RATE 60.0 // 60Hz
#define DOCUMENT_IDEAL_SIZE   10
#define DOCUMENT_WHEEL_LINES  3

DisassemblerTextView::DisassemblerTextView(QWidget *parent): QAbstractScrollArea(parent), m_disassembler(NULL), m_disassemblerpopup(NULL), m_refreshtimerid(-1)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setStyleHint(QFont::TypeWriter);

    int maxwidth = qApp->primaryScreen()->size().width();
    this->viewport()->setFixedWidth(maxwidth);

    this->setFont(font);
    this->setCursor(Qt::ArrowCursor);
    this->setFrameStyle(QFrame::NoFrame);
    this->setContextMenuPolicy(Qt::CustomContextMenu);
    this->setFocusPolicy(Qt::StrongFocus);
    this->verticalScrollBar()->setMinimum(0);
    this->verticalScrollBar()->setValue(0);
    this->verticalScrollBar()->setSingleStep(1);
    this->verticalScrollBar()->setPageStep(1);
    this->horizontalScrollBar()->setSingleStep(this->fontMetrics().boundingRect(" ").width());
    this->horizontalScrollBar()->setMinimum(0);
    this->horizontalScrollBar()->setValue(0);
    this->horizontalScrollBar()->setMaximum(maxwidth);

    float refreshfreq = qApp->primaryScreen()->refreshRate();

    if(refreshfreq <= 0)
        refreshfreq = FALLBACK_REFRESH_RATE;

    REDasm::log("Setting refresh rate to " + QString::number(refreshfreq, 'f', 1).toStdString() + "Hz");
    m_refreshrate = std::ceil((1 / refreshfreq) * 1000);
    m_blinktimerid = this->startTimer(CURSOR_BLINK_INTERVAL);

    connect(this, &DisassemblerTextView::customContextMenuRequested, this, [&](const QPoint&) {
        m_contextmenu->exec(QCursor::pos());
    });

    this->createContextMenu();
}

DisassemblerTextView::~DisassemblerTextView()
{
    if(m_blinktimerid != -1)
    {
        this->killTimer(m_blinktimerid);
        m_blinktimerid = -1;
    }

    if(m_refreshtimerid != -1)
    {
        this->killTimer(m_refreshtimerid);
        m_refreshtimerid = -1;
    }
}

bool DisassemblerTextView::canGoBack() const { return m_disassembler->document()->cursor()->canGoBack(); }
bool DisassemblerTextView::canGoForward() const { return m_disassembler->document()->cursor()->canGoForward(); }

u64 DisassemblerTextView::visibleLines() const
{
    QFontMetrics fm = this->fontMetrics();
    u64 vl = std::ceil(this->height() / fm.height());

    if((vl <= 1) && (m_disassembler->document()->size() >= DOCUMENT_IDEAL_SIZE))
        return DOCUMENT_IDEAL_SIZE;

    return vl;
}

u64 DisassemblerTextView::firstVisibleLine() const { return this->verticalScrollBar()->value(); }
u64 DisassemblerTextView::lastVisibleLine() const { return this->firstVisibleLine() + this->visibleLines() - 1; }

void DisassemblerTextView::setDisassembler(REDasm::DisassemblerAPI *disassembler)
{
    m_disassembler = disassembler;

    REDasm::ListingDocument& document = m_disassembler->document();
    REDasm::ListingCursor* cur = document->cursor();

    EVENT_CONNECT(document, changed, this, std::bind(&DisassemblerTextView::onDocumentChanged, this, std::placeholders::_1));
    EVENT_CONNECT(cur, positionChanged, this, std::bind(&DisassemblerTextView::moveToSelection, this));
    EVENT_CONNECT(cur, backChanged, this, [=]() { emit canGoBackChanged(); });
    EVENT_CONNECT(cur, forwardChanged, this, [=]() { emit canGoForwardChanged(); });

    this->adjustScrollBars();
    connect(this->verticalScrollBar(), &QScrollBar::valueChanged, this, [&](int) { this->renderListing(); });

    m_renderer = std::make_unique<ListingTextRenderer>(this->font(), m_disassembler);
    m_disassemblerpopup = new DisassemblerPopup(m_disassembler, this);

    if(!m_disassembler->busy())
        cur->positionChanged();
}

void DisassemblerTextView::copy()
{
    if(!m_disassembler->document()->cursor()->hasSelection())
        return;

    qApp->clipboard()->setText(S_TO_QS(m_renderer->getSelectedText()));
}

bool DisassemblerTextView::goTo(address_t address)
{
    auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());
    auto it = lock->item(address);

    if(it == lock->end())
        return false;

    this->goTo(it->get());
    return true;
}

void DisassemblerTextView::goTo(REDasm::ListingItem *item)
{
    auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());
    int idx = lock->indexOf(item);

    if(idx == -1)
        return;

    lock->cursor()->moveTo(idx);
}

void DisassemblerTextView::addComment()
{
    REDasm::ListingDocument& document = m_disassembler->document();
    address_t currentaddress = document->currentItem()->address;

    bool ok = false;
    QString res = QInputDialog::getMultiLineText(this,
                                                 "Comment @ " + QString::fromStdString(REDasm::hex(currentaddress)),
                                                 "Insert a comment (leave blank to remove):",
                                                 QString::fromStdString(document->comment(currentaddress, true)), &ok);

    if(!ok)
        return;

    document->comment(currentaddress, res.toStdString());
}

void DisassemblerTextView::printFunctionHexDump()
{
    REDasm::ListingDocument& document = m_disassembler->document();
    REDasm::ListingItem* item = document->currentItem();

    if(!item)
        return;

    REDasm::SymbolPtr symbol = document->functionStartSymbol(item->address);

    if(!symbol)
        return;

    REDasm::BufferView br = m_disassembler->getFunctionBytes(symbol->address);

    if(br.eob())
        return;

    REDasm::log(symbol->name + ":" + REDasm::quoted(REDasm::hexstring(br, br.size())));
}

void DisassemblerTextView::goBack() { m_disassembler->document()->cursor()->goBack();  }
void DisassemblerTextView::goForward() { m_disassembler->document()->cursor()->goForward(); }

void DisassemblerTextView::renderListing(const QRect &r)
{
    if(!m_disassembler || (m_disassembler->busy() && (m_refreshtimerid != -1)))
        return;

    if(r.isNull())
        this->viewport()->update();
    else
        this->viewport()->update(r);

    m_refreshtimerid = this->startTimer(m_refreshrate);
}

void DisassemblerTextView::blinkCursor()
{
    if(!m_disassembler)
        return;

    if(m_disassembler->busy())
    {
        m_renderer->toggleCursor();
        return;
    }

    auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());
    REDasm::ListingCursor* cur = lock->cursor();

    if(!this->hasFocus())
    {
        if(m_renderer->cursorActive())
        {
            m_renderer->disableCursor();
            this->renderLine(cur->currentLine());
        }

        return;
    }

    m_renderer->toggleCursor();
    this->renderLine(cur->currentLine());
}

void DisassemblerTextView::scrollContentsBy(int dx, int dy)
{
    if(dx)
    {
        QWidget* viewport = this->viewport();
        viewport->move(viewport->x() + dx, viewport->y());
        return;
    }

    QAbstractScrollArea::scrollContentsBy(dx, dy);
}

void DisassemblerTextView::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e)

    if(!m_disassembler || !m_renderer)
        return;

    QFontMetrics fm = this->fontMetrics();
    const QRect& r = e->rect();

    u64 firstvisible = this->firstVisibleLine();
    u64 first = firstvisible + (r.top() / fm.height());
    u64 last = firstvisible + (r.bottom() / fm.height());
    u64 count = (last - first) + 1;

    QPainter painter(this->viewport());
    painter.setFont(this->font());

    m_renderer->setFirstVisibleLine(firstvisible);
    m_renderer->render(first, count, &painter);
}

void DisassemblerTextView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    this->adjustScrollBars();
}

void DisassemblerTextView::mousePressEvent(QMouseEvent *e)
{
    REDasm::ListingCursor* cur = m_disassembler->document()->cursor();

    if((e->button() == Qt::LeftButton) || (!cur->hasSelection() && (e->button() == Qt::RightButton)))
    {
        e->accept();
        REDasm::ListingCursor::Position cp = m_renderer->hitTest(e->pos(), this->firstVisibleLine());
        cur->moveTo(cp.first, cp.second);
        m_renderer->highlightWordUnderCursor();
    }

    QAbstractScrollArea::mousePressEvent(e);
}

void DisassemblerTextView::mouseMoveEvent(QMouseEvent *e)
{
    if(e->buttons() == Qt::LeftButton)
    {
        e->accept();
        m_renderer->disableCursor();

        auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());
        REDasm::ListingCursor* cur = lock->cursor();
        REDasm::ListingCursor::Position cp = m_renderer->hitTest(e->pos(), this->firstVisibleLine());
        cur->select(cp.first, cp.second);
        e->accept();
        return;
    }

    QAbstractScrollArea::mouseMoveEvent(e);
}

void DisassemblerTextView::mouseReleaseEvent(QMouseEvent *e)
{
    if(e->button() == Qt::LeftButton)
        e->accept();

    QAbstractScrollArea::mouseReleaseEvent(e);
}

void DisassemblerTextView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if(e->button() == Qt::LeftButton)
    {
        e->accept();

        if(this->followUnderCursor())
            return;

        auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());
        REDasm::ListingCursor* cur = lock->cursor();
        ListingTextRenderer::Range r = m_renderer->wordHitTest(e->pos(), this->firstVisibleLine());

        if(r.first == -1)
            return;

        cur->moveTo(cur->currentLine(), r.first);
        cur->select(cur->currentLine(), r.second);
        return;
    }

    QAbstractScrollArea::mouseReleaseEvent(e);
}

void DisassemblerTextView::wheelEvent(QWheelEvent *e)
{
    if(e->orientation() == Qt::Vertical)
    {
        int value = this->verticalScrollBar()->value();

        if(e->delta() < 0) // Scroll Down
            this->verticalScrollBar()->setValue(value + DOCUMENT_WHEEL_LINES);
        else if(e->delta() > 0) // Scroll Up
            this->verticalScrollBar()->setValue(value - DOCUMENT_WHEEL_LINES);

        return;
    }

    QAbstractScrollArea::wheelEvent(e);
}

void DisassemblerTextView::keyPressEvent(QKeyEvent *e)
{
    auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());
    REDasm::ListingCursor* cur = lock->cursor();

    m_renderer->enableCursor();

    if(e->matches(QKeySequence::MoveToNextChar) || e->matches(QKeySequence::SelectNextChar))
    {
        u64 len = m_renderer->getLastColumn(cur->currentLine());

        if(e->matches(QKeySequence::MoveToNextChar))
            cur->moveTo(cur->currentLine(), std::min(len, cur->currentColumn() + 1));
        else
            cur->select(cur->currentLine(), std::min(len, cur->currentColumn() + 1));
    }
    else if(e->matches(QKeySequence::MoveToPreviousChar) || e->matches(QKeySequence::SelectPreviousChar))
    {
        if(e->matches(QKeySequence::MoveToPreviousChar))
            cur->moveTo(cur->currentLine(), std::max(static_cast<u64>(0), cur->currentColumn() - 1));
        else
            cur->select(cur->currentLine(), std::max(static_cast<u64>(0), cur->currentColumn() - 1));
    }
    else if(e->matches(QKeySequence::MoveToNextLine) || e->matches(QKeySequence::SelectNextLine))
    {
        if(lock->lastLine()  == cur->currentLine())
            return;

        int nextline = cur->currentLine() + 1;

        if(e->matches(QKeySequence::MoveToNextLine))
            cur->moveTo(nextline, std::min(cur->currentColumn(), m_renderer->getLastColumn(nextline)));
        else
            cur->select(nextline, std::min(cur->currentColumn(), m_renderer->getLastColumn(nextline)));
    }
    else if(e->matches(QKeySequence::MoveToPreviousLine) || e->matches(QKeySequence::SelectPreviousLine))
    {
        if(!cur->currentLine())
            return;

        int prevline = cur->currentLine() - 1;

        if(e->matches(QKeySequence::MoveToPreviousLine))
            cur->moveTo(prevline, std::min(cur->currentColumn(), m_renderer->getLastColumn(prevline)));
        else
            cur->select(prevline, std::min(cur->currentColumn(), m_renderer->getLastColumn(prevline)));
    }
    else if(e->matches(QKeySequence::MoveToNextPage) || e->matches(QKeySequence::SelectNextPage))
    {
        if(lock->lastLine()  == cur->currentLine())
            return;

        int pageline = std::min(lock->lastLine(), this->firstVisibleLine() + this->visibleLines());

        if(e->matches(QKeySequence::MoveToNextPage))
            cur->moveTo(pageline, std::min(cur->currentColumn(), m_renderer->getLastColumn(pageline)));
        else
            cur->select(pageline, std::min(cur->currentColumn(), m_renderer->getLastColumn(pageline)));
    }
    else if(e->matches(QKeySequence::MoveToPreviousPage) || e->matches(QKeySequence::SelectPreviousPage))
    {
        if(!cur->currentLine())
            return;

        u64 pageline = 0;

        if(this->firstVisibleLine() > this->visibleLines())
            pageline = std::max(static_cast<u64>(0), this->firstVisibleLine() - this->visibleLines());

        if(e->matches(QKeySequence::MoveToPreviousPage))
            cur->moveTo(pageline, std::min(cur->currentColumn(), m_renderer->getLastColumn(pageline)));
        else
            cur->select(pageline, std::min(cur->currentColumn(), m_renderer->getLastColumn(pageline)));
    }
    else if(e->matches(QKeySequence::MoveToStartOfDocument) || e->matches(QKeySequence::SelectStartOfDocument))
    {
        if(!cur->currentLine())
            return;

        if(e->matches(QKeySequence::MoveToStartOfDocument))
            cur->moveTo(0, 0);
        else
            cur->select(0, 0);
    }
    else if(e->matches(QKeySequence::MoveToEndOfDocument) || e->matches(QKeySequence::SelectEndOfDocument))
    {
        if(lock->lastLine() == cur->currentLine())
            return;

        if(e->matches(QKeySequence::MoveToEndOfDocument))
            cur->moveTo(lock->lastLine(), m_renderer->getLastColumn(lock->lastLine()));
        else
            cur->select(lock->lastLine(), m_renderer->getLastColumn(lock->lastLine()));
    }
    else if(e->matches(QKeySequence::MoveToStartOfLine) || e->matches(QKeySequence::SelectStartOfLine))
    {
        if(e->matches(QKeySequence::MoveToStartOfLine))
            cur->moveTo(cur->currentLine(), 0);
        else
            cur->select(cur->currentLine(), 0);
    }
    else if(e->matches(QKeySequence::MoveToEndOfLine) || e->matches(QKeySequence::SelectEndOfLine))
    {
        if(e->matches(QKeySequence::MoveToEndOfLine))
            cur->moveTo(cur->currentLine(), m_renderer->getLastColumn(cur->currentLine()));
        else
            cur->select(cur->currentLine(), m_renderer->getLastColumn(cur->currentLine()));
    }
    else if(e->key() == Qt::Key_Space)
        emit switchView();
    else
        QAbstractScrollArea::keyPressEvent(e);
}

void DisassemblerTextView::timerEvent(QTimerEvent *e)
{
    if(e->timerId() == m_refreshtimerid)
    {
        this->killTimer(m_refreshtimerid);
        m_refreshtimerid = -1;
        this->renderListing();
    }
    if(e->timerId() == m_blinktimerid)
        this->blinkCursor();

    QAbstractScrollArea::timerEvent(e);
}

bool DisassemblerTextView::event(QEvent *e)
{
    if(m_disassembler && !m_disassembler->busy() && (e->type() == QEvent::ToolTip))
    {
        QHelpEvent* helpevent = static_cast<QHelpEvent*>(e);
        this->showPopup(helpevent->pos());
        return true;
    }

    return QAbstractScrollArea::event(e);
}

void DisassemblerTextView::onDocumentChanged(const REDasm::ListingDocumentChanged *ldc)
{
    m_disassembler->document()->cursor()->clearSelection();
    this->adjustScrollBars();

    if(ldc->action != REDasm::ListingDocumentChanged::Changed) // Insertion or Deletion
    {
        if(ldc->index > this->lastVisibleLine()) // Don't care of bottom Insertion/Deletion
            return;

        QMetaObject::invokeMethod(this, "renderListing", Qt::QueuedConnection);
    }
    else
        QMetaObject::invokeMethod(this, "renderLine", Qt::QueuedConnection, Q_ARG(u64, ldc->index));
}

REDasm::SymbolPtr DisassemblerTextView::symbolUnderCursor()
{
    auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());
    REDasm::ListingCursor* cur = lock->cursor();

    if(!cur->hasWordUnderCursor())
        return NULL;

    return lock->symbol(cur->wordUnderCursor());
}

bool DisassemblerTextView::isLineVisible(u64 line) const
{
    if(line < this->firstVisibleLine())
        return false;

    if(line > this->lastVisibleLine())
        return false;

    return true;
}

bool DisassemblerTextView::isColumnVisible(u64 column, u64 *xpos)
{
    QScrollBar* hscrollbar = this->horizontalScrollBar();
    u64 lastxpos = hscrollbar->value() + this->width();

#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    u64 adv = this->fontMetrics().horizontalAdvance(" ");
#else
    u64 adv = this->fontMetrics().width(" ");
#endif

    *xpos = adv * column;

    if(*xpos > lastxpos)
    {
        *xpos -= this->width();
        return false;
    }
    if(*xpos < this->width())
    {
        *xpos = 0;
        return false;
    }
    if(*xpos < static_cast<u64>(hscrollbar->value()))
    {
        *xpos = hscrollbar->value() - *xpos;
        return false;
    }

    return true;
}

QRect DisassemblerTextView::lineRect(u64 line)
{
    if(!this->isLineVisible(line))
        return QRect();

    QRect vprect = this->viewport()->rect();
    QFontMetrics fm = this->fontMetrics();
    u64 offset = line - this->firstVisibleLine();
    return QRect(vprect.x(), offset * fm.height(), vprect.width(), fm.height());
}

void DisassemblerTextView::renderLine(u64 line)
{
    if(!this->isLineVisible(line))
        return;

    this->renderLines(line, line);
}

void DisassemblerTextView::renderLines(u64 first, u64 last)
{
    first = std::max(first, this->firstVisibleLine());
    last = std::min(last, this->lastVisibleLine());

    QRect firstrect = this->lineRect(first);
    QRect lastrect = this->lineRect(last);

    this->renderListing(QRect(firstrect.topLeft(), lastrect.bottomRight()));
}

void DisassemblerTextView::adjustScrollBars()
{
    if(!m_disassembler)
        return;

    QScrollBar* vscrollbar = this->verticalScrollBar();
    auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());

    if(lock->size() <= static_cast<size_t>(this->visibleLines()))
        vscrollbar->setMaximum(static_cast<int>(lock->size()));
    else
        vscrollbar->setMaximum(static_cast<int>(lock->size() - this->visibleLines() + 1));

    this->ensureColumnVisible();
}

void DisassemblerTextView::moveToSelection()
{
    auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());
    REDasm::ListingCursor* cur = lock->cursor();

    if(this->isLineVisible(cur->currentLine()))
    {
        this->renderListing();

        if(this->isVisible())
            m_renderer->highlightWordUnderCursor();
    }
    else // Center on selection
    {
        QScrollBar* vscrollbar = this->verticalScrollBar();
        vscrollbar->setValue(std::max(static_cast<u64>(0), cur->currentLine() - this->visibleLines() / 2));
    }

    this->ensureColumnVisible();
    REDasm::ListingItem* item = lock->itemAt(cur->currentLine());

    if(item)
        emit addressChanged(item->address);
}

void DisassemblerTextView::createContextMenu()
{
    m_contextmenu = new QMenu(this);
    m_actrename = m_contextmenu->addAction("Rename", this, &DisassemblerTextView::renameCurrentSymbol, QKeySequence(Qt::Key_N));
    m_actcomment = m_contextmenu->addAction("Comment", this, &DisassemblerTextView::addComment, QKeySequence(Qt::Key_Semicolon));
    m_contextmenu->addSeparator();
    m_actxrefs = m_contextmenu->addAction("Cross References", this, &DisassemblerTextView::showReferencesUnderCursor, QKeySequence(Qt::Key_X));
    m_actfollow = m_contextmenu->addAction("Follow", this, &DisassemblerTextView::followUnderCursor);
    m_actfollowpointer = m_contextmenu->addAction("Follow pointer in Hex Dump", this, &DisassemblerTextView::followPointerHexDump);
    m_actgoto = m_contextmenu->addAction("Goto...", this, &DisassemblerTextView::gotoRequested, QKeySequence(Qt::Key_G));
    m_actcallgraph = m_contextmenu->addAction("Call Graph", this, &DisassemblerTextView::showCallGraph, QKeySequence(Qt::CTRL + Qt::Key_G));
    m_contextmenu->addSeparator();
    m_acthexdumpshow = m_contextmenu->addAction("Show Hex Dump", this, &DisassemblerTextView::showHexDump, QKeySequence(Qt::CTRL + Qt::Key_H));
    m_acthexdumpfunc = m_contextmenu->addAction("Hex Dump Function", this, &DisassemblerTextView::printFunctionHexDump);
    m_contextmenu->addSeparator();
    m_actback = m_contextmenu->addAction("Back", this, &DisassemblerTextView::goBack, QKeySequence(Qt::CTRL + Qt::Key_Left));
    m_actforward = m_contextmenu->addAction("Forward", this, &DisassemblerTextView::goForward, QKeySequence(Qt::CTRL + Qt::Key_Right));
    m_contextmenu->addSeparator();
    m_actcopy = m_contextmenu->addAction("Copy", this, &DisassemblerTextView::copy, QKeySequence(QKeySequence::Copy));

    this->addAction(m_actrename);
    this->addAction(m_actxrefs);
    this->addAction(m_actcomment);
    this->addAction(m_actgoto);
    this->addAction(m_actcallgraph);
    this->addAction(m_acthexdumpshow);
    this->addAction(m_actback);
    this->addAction(m_actforward);
    this->addAction(m_actcopy);

    connect(m_contextmenu, &QMenu::aboutToShow, this, &DisassemblerTextView::adjustContextMenu);
}

void DisassemblerTextView::adjustContextMenu()
{
    auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());
    REDasm::SymbolPtr symbol = this->symbolUnderCursor();
    REDasm::ListingItem* item = lock->currentItem();

    if(!item)
        return;

    REDasm::Segment *itemsegment = lock->segment(item->address), *symbolsegment = NULL;
    m_actback->setVisible(this->canGoBack());
    m_actforward->setVisible(this->canGoForward());
    m_actcopy->setVisible(lock->cursor()->hasSelection());

    if(!symbol)
    {
        symbolsegment = lock->segment(item->address);
        symbol = lock->functionStartSymbol(item->address);

        m_actrename->setVisible(false);
        m_actxrefs->setVisible(false);
        m_actfollow->setVisible(false);
        m_actfollowpointer->setVisible(false);

        if(symbol)
            m_actcallgraph->setText(QString("Callgraph %1").arg(S_TO_QS(symbol->name)));

        m_actcallgraph->setVisible(symbol && symbolsegment && symbolsegment->is(REDasm::SegmentTypes::Code));
        m_acthexdumpfunc->setVisible((symbol != nullptr));
        m_acthexdumpshow->setVisible(true);
        return;
    }

    symbolsegment = lock->segment(symbol->address);

    m_actfollowpointer->setVisible(symbol->is(REDasm::SymbolTypes::Pointer));
    m_actfollowpointer->setText(QString("Follow %1 pointer in Hex Dump").arg(S_TO_QS(symbol->name)));

    m_actxrefs->setText(QString("Cross Reference %1").arg(S_TO_QS(symbol->name)));
    m_actxrefs->setVisible(true);

    m_actrename->setText(QString("Rename %1").arg(S_TO_QS(symbol->name)));
    m_actrename->setVisible(!symbol->isLocked());

    m_actcallgraph->setVisible(symbol->isFunction());
    m_actcallgraph->setText(QString("Callgraph %1").arg(S_TO_QS(symbol->name)));

    m_actfollow->setText(QString("Follow %1").arg(S_TO_QS(symbol->name)));
    m_actfollow->setVisible(symbol->is(REDasm::SymbolTypes::Code));

    m_actcomment->setVisible(item->is(REDasm::ListingItem::InstructionItem));

    m_acthexdumpshow->setVisible(symbolsegment && !symbolsegment->is(REDasm::SegmentTypes::Bss));
    m_acthexdumpfunc->setVisible(itemsegment && !itemsegment->is(REDasm::SegmentTypes::Bss) && itemsegment->is(REDasm::SegmentTypes::Code));
}

void DisassemblerTextView::ensureColumnVisible()
{
    if(!m_disassembler)
        return;

    auto lock = REDasm::s_lock_safe_ptr(m_disassembler->document());
    REDasm::ListingCursor* cur = lock->cursor();
    u64 xpos = 0;

    if(this->isColumnVisible(cur->currentColumn(), &xpos))
        return;

    QScrollBar* hscrollbar = this->horizontalScrollBar();
    hscrollbar->setValue(xpos);
}

void DisassemblerTextView::showReferencesUnderCursor()
{
    REDasm::SymbolPtr symbol = this->symbolUnderCursor();

    if(!symbol)
        return;

    emit referencesRequested(symbol->address);
}

bool DisassemblerTextView::followUnderCursor()
{
    REDasm::SymbolPtr symbol = this->symbolUnderCursor();

    if(!symbol)
        return false;

    this->goTo(symbol->address);
    return true;
}

bool DisassemblerTextView::followPointerHexDump()
{
    REDasm::SymbolPtr symbol = this->symbolUnderCursor();

    if(!symbol || !symbol->is(REDasm::SymbolTypes::Pointer))
        return false;

    u64 destination = 0;

    if(!m_disassembler->dereference(symbol->address, &destination) || !m_disassembler->document()->segment(destination))
        return false;

    emit hexDumpRequested(destination, m_disassembler->format()->addressWidth());
    return true;
}

void DisassemblerTextView::showCallGraph()
{
    REDasm::SymbolPtr symbol = this->symbolUnderCursor();

    if(!symbol)
    {
        REDasm::ListingDocument& document = m_disassembler->document();
        REDasm::ListingItem* item = document->currentItem();
        symbol = document->functionStartSymbol(item->address);
    }

    emit callGraphRequested(symbol->address);
}

void DisassemblerTextView::showHexDump()
{
    REDasm::SymbolPtr symbol = this->symbolUnderCursor();

    if(!symbol)
    {
        emit switchToHexDump();
        return;
    }

    u64 len = sizeof(m_disassembler->format()->addressWidth());

    if(symbol->is(REDasm::SymbolTypes::String))
        len = m_disassembler->readString(symbol).size();

    emit hexDumpRequested(symbol->address, len);
}

void DisassemblerTextView::showPopup(const QPoint& pos)
{
    std::string word = m_renderer->getWordUnderCursor(pos, this->firstVisibleLine());

    if(!word.empty())
    {
        REDasm::ListingCursor::Position cp = m_renderer->hitTest(pos, this->firstVisibleLine());
        m_disassemblerpopup->popup(word, cp.first);
        return;
    }

    m_disassemblerpopup->hide();
}

void DisassemblerTextView::renameCurrentSymbol()
{
    REDasm::SymbolPtr symbol = this->symbolUnderCursor();

    if(!symbol || symbol->isLocked())
        return;

    REDasm::ListingDocument& document = m_disassembler->document();

    QString symbolname = S_TO_QS(symbol->name);
    QString res = QInputDialog::getText(this, QString("Rename %1").arg(symbolname), "Symbol name:", QLineEdit::Normal, symbolname);

    if(document->symbol(res.toStdString()))
    {
        QMessageBox::warning(this, "Rename failed", "Duplicate symbol name");
        this->renameCurrentSymbol();
        return;
    }

    document->rename(symbol->address, res.toStdString());
    this->renderListing();
}
