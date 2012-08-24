/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2012 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: http://www.qt-project.org/
**
**
** GNU Lesser General Public License Usage
**
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this file.
** Please review the following information to ensure the GNU Lesser General
** Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** Other Usage
**
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**************************************************************************/

#include "qmakeevaluator.h"

#include "qmakeglobals.h"
#include "qmakeparser.h"
#include "qmakeevaluator_p.h"
#include "ioutils.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRegExp>
#include <QSet>
#include <QStack>
#include <QString>
#include <QStringList>
#ifdef PROEVALUATOR_THREAD_SAFE
# include <QThreadPool>
#endif

#ifdef Q_OS_UNIX
#include <unistd.h>
#include <sys/utsname.h>
#else
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>

using namespace ProFileEvaluatorInternal;

QT_BEGIN_NAMESPACE

#define fL1S(s) QString::fromLatin1(s)


QMakeBaseKey::QMakeBaseKey(const QString &_root, bool _hostBuild)
    : root(_root), hostBuild(_hostBuild)
{
}

uint qHash(const QMakeBaseKey &key)
{
    return qHash(key.root) ^ (uint)key.hostBuild;
}

bool operator==(const QMakeBaseKey &one, const QMakeBaseKey &two)
{
    return one.root == two.root && one.hostBuild == two.hostBuild;
}

QMakeBaseEnv::QMakeBaseEnv()
    : evaluator(0)
{
#ifdef PROEVALUATOR_THREAD_SAFE
    inProgress = false;
#endif
}

QMakeBaseEnv::~QMakeBaseEnv()
{
    delete evaluator;
}

namespace ProFileEvaluatorInternal {
QMakeStatics statics;
}

void QMakeEvaluator::initStatics()
{
    if (!statics.field_sep.isNull())
        return;

    statics.field_sep = QLatin1String(" ");
    statics.strtrue = QLatin1String("true");
    statics.strfalse = QLatin1String("false");
    statics.strCONFIG = ProKey("CONFIG");
    statics.strARGS = ProKey("ARGS");
    statics.strDot = QLatin1String(".");
    statics.strDotDot = QLatin1String("..");
    statics.strever = QLatin1String("ever");
    statics.strforever = QLatin1String("forever");
    statics.strhost_build = QLatin1String("host_build");
    statics.strTEMPLATE = ProKey("TEMPLATE");
#ifdef PROEVALUATOR_FULL
    statics.strREQUIRES = ProKey("REQUIRES");
#endif

    statics.fakeValue = ProStringList(ProString("_FAKE_")); // It has to have a unique begin() value

    initFunctionStatics();

    static const struct {
        const char * const oldname, * const newname;
    } mapInits[] = {
        { "INTERFACES", "FORMS" },
        { "QMAKE_POST_BUILD", "QMAKE_POST_LINK" },
        { "TARGETDEPS", "POST_TARGETDEPS" },
        { "LIBPATH", "QMAKE_LIBDIR" },
        { "QMAKE_EXT_MOC", "QMAKE_EXT_CPP_MOC" },
        { "QMAKE_MOD_MOC", "QMAKE_H_MOD_MOC" },
        { "QMAKE_LFLAGS_SHAPP", "QMAKE_LFLAGS_APP" },
        { "PRECOMPH", "PRECOMPILED_HEADER" },
        { "PRECOMPCPP", "PRECOMPILED_SOURCE" },
        { "INCPATH", "INCLUDEPATH" },
        { "QMAKE_EXTRA_WIN_COMPILERS", "QMAKE_EXTRA_COMPILERS" },
        { "QMAKE_EXTRA_UNIX_COMPILERS", "QMAKE_EXTRA_COMPILERS" },
        { "QMAKE_EXTRA_WIN_TARGETS", "QMAKE_EXTRA_TARGETS" },
        { "QMAKE_EXTRA_UNIX_TARGETS", "QMAKE_EXTRA_TARGETS" },
        { "QMAKE_EXTRA_UNIX_INCLUDES", "QMAKE_EXTRA_INCLUDES" },
        { "QMAKE_EXTRA_UNIX_VARIABLES", "QMAKE_EXTRA_VARIABLES" },
        { "QMAKE_RPATH", "QMAKE_LFLAGS_RPATH" },
        { "QMAKE_FRAMEWORKDIR", "QMAKE_FRAMEWORKPATH" },
        { "QMAKE_FRAMEWORKDIR_FLAGS", "QMAKE_FRAMEWORKPATH_FLAGS" },
        { "IN_PWD", "PWD" }
    };
    for (unsigned i = 0; i < sizeof(mapInits)/sizeof(mapInits[0]); ++i)
        statics.varMap.insert(ProKey(mapInits[i].oldname), ProKey(mapInits[i].newname));
}

const ProKey &QMakeEvaluator::map(const ProKey &var)
{
    QHash<ProKey, ProKey>::ConstIterator it = statics.varMap.constFind(var);
    if (it == statics.varMap.constEnd())
        return var;
    deprecationWarning(fL1S("Variable %1 is deprecated; use %2 instead.")
                       .arg(var.toQString(), it.value().toQString()));
    return it.value();
}


QMakeEvaluator::QMakeEvaluator(QMakeGlobals *option,
                               QMakeParser *parser, QMakeHandler *handler)
  : m_option(option), m_parser(parser), m_handler(handler)
{
    // So that single-threaded apps don't have to call initialize() for now.
    initStatics();

    // Configuration, more or less
    m_caller = 0;
#ifdef PROEVALUATOR_CUMULATIVE
    m_cumulative = false;
#endif
    m_hostBuild = false;

    // Evaluator state
#ifdef PROEVALUATOR_CUMULATIVE
    m_skipLevel = 0;
#endif
    m_loopLevel = 0;
    m_listCount = 0;
    m_valuemapStack.push(ProValueMap());
    m_valuemapInited = false;
}

QMakeEvaluator::~QMakeEvaluator()
{
}

void QMakeEvaluator::initFrom(const QMakeEvaluator &other)
{
    Q_ASSERT_X(&other, "QMakeEvaluator::visitProFile", "Project not prepared");
    m_functionDefs = other.m_functionDefs;
    m_valuemapStack = other.m_valuemapStack;
    m_valuemapInited = true;
    m_qmakespec = other.m_qmakespec;
    m_qmakespecFull = other.m_qmakespecFull;
    m_qmakespecName = other.m_qmakespecName;
    m_mkspecPaths = other.m_mkspecPaths;
    m_featureRoots = other.m_featureRoots;
}

//////// Evaluator tools /////////

uint QMakeEvaluator::getBlockLen(const ushort *&tokPtr)
{
    uint len = *tokPtr++;
    len |= (uint)*tokPtr++ << 16;
    return len;
}

ProString QMakeEvaluator::getStr(const ushort *&tokPtr)
{
    uint len = *tokPtr++;
    ProString ret(m_current.pro->items(), tokPtr - m_current.pro->tokPtr(), len);
    ret.setSource(m_current.pro);
    tokPtr += len;
    return ret;
}

ProKey QMakeEvaluator::getHashStr(const ushort *&tokPtr)
{
    uint hash = getBlockLen(tokPtr);
    uint len = *tokPtr++;
    ProKey ret(m_current.pro->items(), tokPtr - m_current.pro->tokPtr(), len, hash);
    tokPtr += len;
    return ret;
}

void QMakeEvaluator::skipStr(const ushort *&tokPtr)
{
    uint len = *tokPtr++;
    tokPtr += len;
}

void QMakeEvaluator::skipHashStr(const ushort *&tokPtr)
{
    tokPtr += 2;
    uint len = *tokPtr++;
    tokPtr += len;
}

// FIXME: this should not build new strings for direct sections.
// Note that the E_SPRINTF and E_LIST implementations rely on the deep copy.
ProStringList QMakeEvaluator::split_value_list(const QString &vals, const ProFile *source)
{
    QString build;
    ProStringList ret;
    QStack<char> quote;

    const ushort SPACE = ' ';
    const ushort LPAREN = '(';
    const ushort RPAREN = ')';
    const ushort SINGLEQUOTE = '\'';
    const ushort DOUBLEQUOTE = '"';
    const ushort BACKSLASH = '\\';

    if (!source)
        source = currentProFile();

    ushort unicode;
    const QChar *vals_data = vals.data();
    const int vals_len = vals.length();
    int parens = 0;
    for (int x = 0; x < vals_len; x++) {
        unicode = vals_data[x].unicode();
        if (x != (int)vals_len-1 && unicode == BACKSLASH &&
            (vals_data[x+1].unicode() == SINGLEQUOTE || vals_data[x+1].unicode() == DOUBLEQUOTE)) {
            build += vals_data[x++]; //get that 'escape'
        } else if (!quote.isEmpty() && unicode == quote.top()) {
            quote.pop();
        } else if (unicode == SINGLEQUOTE || unicode == DOUBLEQUOTE) {
            quote.push(unicode);
        } else if (unicode == RPAREN) {
            --parens;
        } else if (unicode == LPAREN) {
            ++parens;
        }

        if (!parens && quote.isEmpty() && vals_data[x] == SPACE) {
            ret << ProString(build).setSource(source);
            build.clear();
        } else {
            build += vals_data[x];
        }
    }
    if (!build.isEmpty())
        ret << ProString(build).setSource(source);
    if (parens)
        deprecationWarning(fL1S("Unmatched parentheses are deprecated."));
    return ret;
}

static void zipEmpty(ProStringList *value)
{
    for (int i = value->size(); --i >= 0;)
        if (value->at(i).isEmpty())
            value->remove(i);
}

static void insertUnique(ProStringList *varlist, const ProStringList &value)
{
    foreach (const ProString &str, value)
        if (!str.isEmpty() && !varlist->contains(str))
            varlist->append(str);
}

static void removeAll(ProStringList *varlist, const ProString &value)
{
    for (int i = varlist->size(); --i >= 0; )
        if (varlist->at(i) == value)
            varlist->remove(i);
}

void QMakeEvaluator::removeEach(ProStringList *varlist, const ProStringList &value)
{
    foreach (const ProString &str, value)
        if (!str.isEmpty())
            removeAll(varlist, str);
}

static void replaceInList(ProStringList *varlist,
        const QRegExp &regexp, const QString &replace, bool global, QString &tmp)
{
    for (ProStringList::Iterator varit = varlist->begin(); varit != varlist->end(); ) {
        QString val = varit->toQString(tmp);
        QString copy = val; // Force detach and have a reference value
        val.replace(regexp, replace);
        if (!val.isSharedWith(copy)) {
            if (val.isEmpty()) {
                varit = varlist->erase(varit);
            } else {
                (*varit).setValue(val);
                ++varit;
            }
            if (!global)
                break;
        } else {
            ++varit;
        }
    }
}

//////// Evaluator /////////

static ALWAYS_INLINE void addStr(
        const ProString &str, ProStringList *ret, bool &pending, bool joined)
{
    if (joined) {
        ret->last().append(str, &pending);
    } else {
        if (!pending) {
            pending = true;
            *ret << str;
        } else {
            ret->last().append(str);
        }
    }
}

static ALWAYS_INLINE void addStrList(
        const ProStringList &list, ushort tok, ProStringList *ret, bool &pending, bool joined)
{
    if (!list.isEmpty()) {
        if (joined) {
            ret->last().append(list, &pending, !(tok & TokQuoted));
        } else {
            if (tok & TokQuoted) {
                if (!pending) {
                    pending = true;
                    *ret << ProString();
                }
                ret->last().append(list);
            } else {
                if (!pending) {
                    // Another qmake bizzarity: if nothing is pending and the
                    // first element is empty, it will be eaten
                    if (!list.at(0).isEmpty()) {
                        // The common case
                        pending = true;
                        *ret += list;
                        return;
                    }
                } else {
                    ret->last().append(list.at(0));
                }
                // This is somewhat slow, but a corner case
                for (int j = 1; j < list.size(); ++j) {
                    pending = true;
                    *ret << list.at(j);
                }
            }
        }
    }
}

void QMakeEvaluator::evaluateExpression(
        const ushort *&tokPtr, ProStringList *ret, bool joined)
{
    if (joined)
        *ret << ProString();
    bool pending = false;
    forever {
        ushort tok = *tokPtr++;
        if (tok & TokNewStr)
            pending = false;
        ushort maskedTok = tok & TokMask;
        switch (maskedTok) {
        case TokLine:
            m_current.line = *tokPtr++;
            break;
        case TokLiteral:
            addStr(getStr(tokPtr), ret, pending, joined);
            break;
        case TokHashLiteral:
            addStr(getHashStr(tokPtr), ret, pending, joined);
            break;
        case TokVariable:
            addStrList(values(map(getHashStr(tokPtr))), tok, ret, pending, joined);
            break;
        case TokProperty:
            addStr(propertyValue(getHashStr(tokPtr)).setSource(currentProFile()),
                   ret, pending, joined);
            break;
        case TokEnvVar:
            addStrList(split_value_list(m_option->getEnv(getStr(tokPtr).toQString(m_tmp1))),
                       tok, ret, pending, joined);
            break;
        case TokFuncName: {
            const ProKey &func = getHashStr(tokPtr);
            addStrList(evaluateExpandFunction(func, tokPtr), tok, ret, pending, joined);
            break; }
        default:
            tokPtr--;
            return;
        }
    }
}

void QMakeEvaluator::skipExpression(const ushort *&pTokPtr)
{
    const ushort *tokPtr = pTokPtr;
    forever {
        ushort tok = *tokPtr++;
        switch (tok) {
        case TokLine:
            m_current.line = *tokPtr++;
            break;
        case TokValueTerminator:
        case TokFuncTerminator:
            pTokPtr = tokPtr;
            return;
        case TokArgSeparator:
            break;
        default:
            switch (tok & TokMask) {
            case TokLiteral:
            case TokEnvVar:
                skipStr(tokPtr);
                break;
            case TokHashLiteral:
            case TokVariable:
            case TokProperty:
                skipHashStr(tokPtr);
                break;
            case TokFuncName:
                skipHashStr(tokPtr);
                pTokPtr = tokPtr;
                skipExpression(pTokPtr);
                tokPtr = pTokPtr;
                break;
            default:
                Q_ASSERT_X(false, "skipExpression", "Unrecognized token");
                break;
            }
        }
    }
}

QMakeEvaluator::VisitReturn QMakeEvaluator::visitProBlock(
        ProFile *pro, const ushort *tokPtr)
{
    m_current.pro = pro;
    m_current.line = 0;
    return visitProBlock(tokPtr);
}

QMakeEvaluator::VisitReturn QMakeEvaluator::visitProBlock(
        const ushort *tokPtr)
{
    ProStringList curr;
    bool okey = true, or_op = false, invert = false;
    uint blockLen;
    while (ushort tok = *tokPtr++) {
        VisitReturn ret;
        switch (tok) {
        case TokLine:
            m_current.line = *tokPtr++;
            continue;
        case TokAssign:
        case TokAppend:
        case TokAppendUnique:
        case TokRemove:
        case TokReplace:
            visitProVariable(tok, curr, tokPtr);
            curr.clear();
            continue;
        case TokBranch:
            blockLen = getBlockLen(tokPtr);
            if (m_cumulative) {
#ifdef PROEVALUATOR_CUMULATIVE
                if (!okey)
                    m_skipLevel++;
                ret = blockLen ? visitProBlock(tokPtr) : ReturnTrue;
                tokPtr += blockLen;
                blockLen = getBlockLen(tokPtr);
                if (!okey)
                    m_skipLevel--;
                else
                    m_skipLevel++;
                if ((ret == ReturnTrue || ret == ReturnFalse) && blockLen)
                    ret = visitProBlock(tokPtr);
                if (okey)
                    m_skipLevel--;
#endif
            } else {
                if (okey)
                    ret = blockLen ? visitProBlock(tokPtr) : ReturnTrue;
                tokPtr += blockLen;
                blockLen = getBlockLen(tokPtr);
                if (!okey)
                    ret = blockLen ? visitProBlock(tokPtr) : ReturnTrue;
            }
            tokPtr += blockLen;
            okey = true, or_op = false; // force next evaluation
            break;
        case TokForLoop:
            if (m_cumulative) { // This is a no-win situation, so just pretend it's no loop
                skipHashStr(tokPtr);
                uint exprLen = getBlockLen(tokPtr);
                tokPtr += exprLen;
                blockLen = getBlockLen(tokPtr);
                ret = visitProBlock(tokPtr);
            } else if (okey != or_op) {
                const ProKey &variable = getHashStr(tokPtr);
                uint exprLen = getBlockLen(tokPtr);
                const ushort *exprPtr = tokPtr;
                tokPtr += exprLen;
                blockLen = getBlockLen(tokPtr);
                ret = visitProLoop(variable, exprPtr, tokPtr);
            } else {
                skipHashStr(tokPtr);
                uint exprLen = getBlockLen(tokPtr);
                tokPtr += exprLen;
                blockLen = getBlockLen(tokPtr);
                ret = ReturnTrue;
            }
            tokPtr += blockLen;
            okey = true, or_op = false; // force next evaluation
            break;
        case TokTestDef:
        case TokReplaceDef:
            if (m_cumulative || okey != or_op) {
                const ProKey &name = getHashStr(tokPtr);
                blockLen = getBlockLen(tokPtr);
                visitProFunctionDef(tok, name, tokPtr);
            } else {
                skipHashStr(tokPtr);
                blockLen = getBlockLen(tokPtr);
            }
            tokPtr += blockLen;
            okey = true, or_op = false; // force next evaluation
            continue;
        case TokNot:
            invert ^= true;
            continue;
        case TokAnd:
            or_op = false;
            continue;
        case TokOr:
            or_op = true;
            continue;
        case TokCondition:
            if (!m_skipLevel && okey != or_op) {
                if (curr.size() != 1) {
                    if (!m_cumulative || !curr.isEmpty())
                        evalError(fL1S("Conditional must expand to exactly one word."));
                    okey = false;
                } else {
                    okey = isActiveConfig(curr.at(0).toQString(m_tmp2), true) ^ invert;
                }
            }
            or_op = !okey; // tentatively force next evaluation
            invert = false;
            curr.clear();
            continue;
        case TokTestCall:
            if (!m_skipLevel && okey != or_op) {
                if (curr.size() != 1) {
                    if (!m_cumulative || !curr.isEmpty())
                        evalError(fL1S("Test name must expand to exactly one word."));
                    skipExpression(tokPtr);
                    okey = false;
                } else {
                    ret = evaluateConditionalFunction(curr.at(0).toKey(), tokPtr);
                    switch (ret) {
                    case ReturnTrue: okey = true; break;
                    case ReturnFalse: okey = false; break;
                    default: return ret;
                    }
                    okey ^= invert;
                }
            } else if (m_cumulative) {
#ifdef PROEVALUATOR_CUMULATIVE
                m_skipLevel++;
                if (curr.size() != 1)
                    skipExpression(tokPtr);
                else
                    evaluateConditionalFunction(curr.at(0).toKey(), tokPtr);
                m_skipLevel--;
#endif
            } else {
                skipExpression(tokPtr);
            }
            or_op = !okey; // tentatively force next evaluation
            invert = false;
            curr.clear();
            continue;
        default: {
                const ushort *oTokPtr = --tokPtr;
                evaluateExpression(tokPtr, &curr, false);
                if (tokPtr != oTokPtr)
                    continue;
            }
            Q_ASSERT_X(false, "visitProBlock", "unexpected item type");
            continue;
        }
        if (ret != ReturnTrue && ret != ReturnFalse)
            return ret;
    }
    return returnBool(okey);
}


void QMakeEvaluator::visitProFunctionDef(
        ushort tok, const ProKey &name, const ushort *tokPtr)
{
    QHash<ProKey, ProFunctionDef> *hash =
            (tok == TokTestDef
             ? &m_functionDefs.testFunctions
             : &m_functionDefs.replaceFunctions);
    hash->insert(name, ProFunctionDef(m_current.pro, tokPtr - m_current.pro->tokPtr()));
}

QMakeEvaluator::VisitReturn QMakeEvaluator::visitProLoop(
        const ProKey &_variable, const ushort *exprPtr, const ushort *tokPtr)
{
    VisitReturn ret = ReturnTrue;
    bool infinite = false;
    int index = 0;
    ProKey variable;
    ProStringList oldVarVal;
    ProString it_list = expandVariableReferences(exprPtr, 0, true).at(0);
    if (_variable.isEmpty()) {
        if (it_list != statics.strever) {
            evalError(fL1S("Invalid loop expression."));
            return ReturnFalse;
        }
        it_list = ProString(statics.strforever);
    } else {
        variable = map(_variable);
        oldVarVal = values(variable);
    }
    ProStringList list = values(it_list.toKey());
    if (list.isEmpty()) {
        if (it_list == statics.strforever) {
            infinite = true;
        } else {
            const QString &itl = it_list.toQString(m_tmp1);
            int dotdot = itl.indexOf(statics.strDotDot);
            if (dotdot != -1) {
                bool ok;
                int start = itl.left(dotdot).toInt(&ok);
                if (ok) {
                    int end = itl.mid(dotdot+2).toInt(&ok);
                    if (ok) {
                        if (start < end) {
                            for (int i = start; i <= end; i++)
                                list << ProString(QString::number(i));
                        } else {
                            for (int i = start; i >= end; i--)
                                list << ProString(QString::number(i));
                        }
                    }
                }
            }
        }
    }

    m_loopLevel++;
    forever {
        if (infinite) {
            if (!variable.isEmpty())
                m_valuemapStack.top()[variable] = ProStringList(ProString(QString::number(index++)));
            if (index > 1000) {
                evalError(fL1S("Ran into infinite loop (> 1000 iterations)."));
                break;
            }
        } else {
            ProString val;
            do {
                if (index >= list.count())
                    goto do_break;
                val = list.at(index++);
            } while (val.isEmpty()); // stupid, but qmake is like that
            m_valuemapStack.top()[variable] = ProStringList(val);
        }

        ret = visitProBlock(tokPtr);
        switch (ret) {
        case ReturnTrue:
        case ReturnFalse:
            break;
        case ReturnNext:
            ret = ReturnTrue;
            break;
        case ReturnBreak:
            ret = ReturnTrue;
            goto do_break;
        default:
            goto do_break;
        }
    }
  do_break:
    m_loopLevel--;

    if (!variable.isEmpty())
        m_valuemapStack.top()[variable] = oldVarVal;
    return ret;
}

void QMakeEvaluator::visitProVariable(
        ushort tok, const ProStringList &curr, const ushort *&tokPtr)
{
    int sizeHint = *tokPtr++;

    if (curr.size() != 1) {
        skipExpression(tokPtr);
        if (!m_cumulative || !curr.isEmpty())
            evalError(fL1S("Left hand side of assignment must expand to exactly one word."));
        return;
    }
    const ProKey &varName = map(curr.first());

    if (tok == TokReplace) {      // ~=
        // DEFINES ~= s/a/b/?[gqi]

        const ProStringList &varVal = expandVariableReferences(tokPtr, sizeHint, true);
        const QString &val = varVal.at(0).toQString(m_tmp1);
        if (val.length() < 4 || val.at(0) != QLatin1Char('s')) {
            evalError(fL1S("The ~= operator can handle only the s/// function."));
            return;
        }
        QChar sep = val.at(1);
        QStringList func = val.split(sep);
        if (func.count() < 3 || func.count() > 4) {
            evalError(fL1S("The s/// function expects 3 or 4 arguments."));
            return;
        }

        bool global = false, quote = false, case_sense = false;
        if (func.count() == 4) {
            global = func[3].indexOf(QLatin1Char('g')) != -1;
            case_sense = func[3].indexOf(QLatin1Char('i')) == -1;
            quote = func[3].indexOf(QLatin1Char('q')) != -1;
        }
        QString pattern = func[1];
        QString replace = func[2];
        if (quote)
            pattern = QRegExp::escape(pattern);

        QRegExp regexp(pattern, case_sense ? Qt::CaseSensitive : Qt::CaseInsensitive);

        // We could make a union of modified and unmodified values,
        // but this will break just as much as it fixes, so leave it as is.
        replaceInList(&valuesRef(varName), regexp, replace, global, m_tmp2);
    } else {
        ProStringList varVal = expandVariableReferences(tokPtr, sizeHint);
        switch (tok) {
        default: // whatever - cannot happen
        case TokAssign:          // =
            zipEmpty(&varVal);
            if (!m_cumulative) {
                // FIXME: add check+warning about accidental value removal.
                // This may be a bit too noisy, though.
                m_valuemapStack.top()[varName] = varVal;
            } else {
                if (!varVal.isEmpty()) {
                    // We are greedy for values. But avoid exponential growth.
                    ProStringList &v = valuesRef(varName);
                    if (v.isEmpty()) {
                        v = varVal;
                    } else {
                        ProStringList old = v;
                        v = varVal;
                        QSet<ProString> has;
                        has.reserve(v.size());
                        foreach (const ProString &s, v)
                            has.insert(s);
                        v.reserve(v.size() + old.size());
                        foreach (const ProString &s, old)
                            if (!has.contains(s))
                                v << s;
                    }
                }
            }
            break;
        case TokAppendUnique:    // *=
            insertUnique(&valuesRef(varName), varVal);
            break;
        case TokAppend:          // +=
            zipEmpty(&varVal);
            valuesRef(varName) += varVal;
            break;
        case TokRemove:       // -=
            if (!m_cumulative) {
                removeEach(&valuesRef(varName), varVal);
            } else {
                // We are stingy with our values, too.
            }
            break;
        }
    }

    if (varName == statics.strTEMPLATE)
        setTemplate();
#ifdef PROEVALUATOR_FULL
    else if (varName == statics.strREQUIRES)
        checkRequirements(values(varName));
#endif
}

void QMakeEvaluator::setTemplate()
{
    ProStringList &values = valuesRef(statics.strTEMPLATE);
    if (!m_option->user_template.isEmpty()) {
        // Don't allow override
        values = ProStringList(ProString(m_option->user_template));
    } else {
        if (values.isEmpty())
            values.append(ProString("app"));
        else
            values.erase(values.begin() + 1, values.end());
    }
    if (!m_option->user_template_prefix.isEmpty()) {
        QString val = values.first().toQString(m_tmp1);
        if (!val.startsWith(m_option->user_template_prefix)) {
            val.prepend(m_option->user_template_prefix);
            values = ProStringList(ProString(val));
        }
    }
}

void QMakeEvaluator::loadDefaults()
{
    ProValueMap &vars = m_valuemapStack.top();

    vars[ProKey("DIR_SEPARATOR")] << ProString(m_option->dir_sep);
    vars[ProKey("DIRLIST_SEPARATOR")] << ProString(m_option->dirlist_sep);
    vars[ProKey("_DATE_")] << ProString(QDateTime::currentDateTime().toString());
    if (!m_option->qmake_abslocation.isEmpty())
        vars[ProKey("QMAKE_QMAKE")] << ProString(m_option->qmake_abslocation);
#if defined(Q_OS_WIN32)
    vars[ProKey("QMAKE_HOST.os")] << ProString("Windows");

    DWORD name_length = 1024;
    wchar_t name[1024];
    if (GetComputerName(name, &name_length))
        vars[ProKey("QMAKE_HOST.name")] << ProString(QString::fromWCharArray(name));

    QSysInfo::WinVersion ver = QSysInfo::WindowsVersion;
    vars[ProKey("QMAKE_HOST.version")] << ProString(QString::number(ver));
    ProString verStr;
    switch (ver) {
    case QSysInfo::WV_Me: verStr = ProString("WinMe"); break;
    case QSysInfo::WV_95: verStr = ProString("Win95"); break;
    case QSysInfo::WV_98: verStr = ProString("Win98"); break;
    case QSysInfo::WV_NT: verStr = ProString("WinNT"); break;
    case QSysInfo::WV_2000: verStr = ProString("Win2000"); break;
    case QSysInfo::WV_2003: verStr = ProString("Win2003"); break;
    case QSysInfo::WV_XP: verStr = ProString("WinXP"); break;
    case QSysInfo::WV_VISTA: verStr = ProString("WinVista"); break;
    default: verStr = ProString("Unknown"); break;
    }
    vars[ProKey("QMAKE_HOST.version_string")] << verStr;

    SYSTEM_INFO info;
    GetSystemInfo(&info);
    ProString archStr;
    switch (info.wProcessorArchitecture) {
# ifdef PROCESSOR_ARCHITECTURE_AMD64
    case PROCESSOR_ARCHITECTURE_AMD64:
        archStr = ProString("x86_64");
        break;
# endif
    case PROCESSOR_ARCHITECTURE_INTEL:
        archStr = ProString("x86");
        break;
    case PROCESSOR_ARCHITECTURE_IA64:
# ifdef PROCESSOR_ARCHITECTURE_IA32_ON_WIN64
    case PROCESSOR_ARCHITECTURE_IA32_ON_WIN64:
# endif
        archStr = ProString("IA64");
        break;
    default:
        archStr = ProString("Unknown");
        break;
    }
    vars[ProKey("QMAKE_HOST.arch")] << archStr;

# if defined(Q_CC_MSVC) // ### bogus condition, but nobody x-builds for msvc with a different qmake
    QLatin1Char backslash('\\');
    QString paths = m_option->getEnv(QLatin1String("PATH"));
    QString vcBin64 = m_option->getEnv(QLatin1String("VCINSTALLDIR"));
    if (!vcBin64.endsWith(backslash))
        vcBin64.append(backslash);
    vcBin64.append(QLatin1String("bin\\amd64"));
    QString vcBinX86_64 = m_option->getEnv(QLatin1String("VCINSTALLDIR"));
    if (!vcBinX86_64.endsWith(backslash))
        vcBinX86_64.append(backslash);
    vcBinX86_64.append(QLatin1String("bin\\x86_amd64"));
    if (paths.contains(vcBin64, Qt::CaseInsensitive)
            || paths.contains(vcBinX86_64, Qt::CaseInsensitive))
        vars[ProKey("QMAKE_TARGET.arch")] << ProString("x86_64");
    else
        vars[ProKey("QMAKE_TARGET.arch")] << ProString("x86");
# endif
#elif defined(Q_OS_UNIX)
    struct utsname name;
    if (!uname(&name)) {
        vars[ProKey("QMAKE_HOST.os")] << ProString(name.sysname);
        vars[ProKey("QMAKE_HOST.name")] << ProString(QString::fromLocal8Bit(name.nodename));
        vars[ProKey("QMAKE_HOST.version")] << ProString(name.release);
        vars[ProKey("QMAKE_HOST.version_string")] << ProString(name.version);
        vars[ProKey("QMAKE_HOST.arch")] << ProString(name.machine);
    }
#endif

    m_valuemapInited = true;
}

bool QMakeEvaluator::prepareProject(const QString &inDir)
{
    QString superdir;
    if (m_option->do_cache) {
        QString conffile;
        QString cachefile = m_option->cachefile;
        if (cachefile.isEmpty())  { //find it as it has not been specified
            if (m_outputDir.isEmpty())
                goto no_cache;
            superdir = m_outputDir;
            forever {
                QString superfile = superdir + QLatin1String("/.qmake.super");
                if (IoUtils::exists(superfile)) {
                    m_superfile = superfile;
                    break;
                }
                QFileInfo qdfi(superdir);
                if (qdfi.isRoot()) {
                    superdir.clear();
                    break;
                }
                superdir = qdfi.path();
            }
            QString sdir = inDir;
            QString dir = m_outputDir;
            forever {
                conffile = sdir + QLatin1String("/.qmake.conf");
                if (!IoUtils::exists(conffile))
                    conffile.clear();
                cachefile = dir + QLatin1String("/.qmake.cache");
                if (!IoUtils::exists(cachefile))
                    cachefile.clear();
                if (!conffile.isEmpty() || !cachefile.isEmpty()) {
                    if (dir != sdir)
                        m_sourceRoot = sdir;
                    m_buildRoot = dir;
                    break;
                }
                if (dir == superdir)
                    goto no_cache;
                QFileInfo qsdfi(sdir);
                QFileInfo qdfi(dir);
                if (qsdfi.isRoot() || qdfi.isRoot())
                    goto no_cache;
                sdir = qsdfi.path();
                dir = qdfi.path();
            }
        } else {
            m_buildRoot = QFileInfo(cachefile).path();
        }
        m_conffile = conffile;
        m_cachefile = cachefile;
    }
  no_cache:

    // Look for mkspecs/ in source and build. First to win determines the root.
    QString sdir = inDir;
    QString dir = m_outputDir;
    while (dir != m_buildRoot) {
        if ((dir != sdir && QFileInfo(sdir, QLatin1String("mkspecs")).isDir())
                || QFileInfo(dir, QLatin1String("mkspecs")).isDir()) {
            if (dir != sdir)
                m_sourceRoot = sdir;
            m_buildRoot = dir;
            break;
        }
        if (dir == superdir)
            break;
        QFileInfo qsdfi(sdir);
        QFileInfo qdfi(dir);
        if (qsdfi.isRoot() || qdfi.isRoot())
            break;
        sdir = qsdfi.path();
        dir = qdfi.path();
    }

    return true;
}

bool QMakeEvaluator::loadSpec()
{
    QString qmakespec = m_option->expandEnvVars(
                m_hostBuild ? m_option->qmakespec : m_option->xqmakespec);

    {
        QMakeEvaluator evaluator(m_option, m_parser, m_handler);
        if (!m_superfile.isEmpty()) {
            valuesRef(ProKey("_QMAKE_SUPER_CACHE_")) << ProString(m_superfile);
            if (!evaluator.evaluateFileDirect(m_superfile, QMakeHandler::EvalConfigFile, LoadProOnly))
                return false;
        }
        if (!m_conffile.isEmpty()) {
            valuesRef(ProKey("_QMAKE_CONF_")) << ProString(m_conffile);
            if (!evaluator.evaluateFileDirect(m_conffile, QMakeHandler::EvalConfigFile, LoadProOnly))
                return false;
        }
        if (!m_cachefile.isEmpty()) {
            valuesRef(ProKey("_QMAKE_CACHE_")) << ProString(m_cachefile);
            if (!evaluator.evaluateFileDirect(m_cachefile, QMakeHandler::EvalConfigFile, LoadProOnly))
                return false;
        }
        if (qmakespec.isEmpty()) {
            if (!m_hostBuild)
                qmakespec = evaluator.first(ProKey("XQMAKESPEC")).toQString();
            if (qmakespec.isEmpty())
                qmakespec = evaluator.first(ProKey("QMAKESPEC")).toQString();
        }
        m_qmakepath = evaluator.values(ProKey("QMAKEPATH")).toQStringList();
        m_qmakefeatures = evaluator.values(ProKey("QMAKEFEATURES")).toQStringList();
    }

    updateMkspecPaths();
    if (qmakespec.isEmpty())
        qmakespec = m_hostBuild ? QLatin1String("default-host") : QLatin1String("default");
    if (IoUtils::isRelativePath(qmakespec)) {
        foreach (const QString &root, m_mkspecPaths) {
            QString mkspec = root + QLatin1Char('/') + qmakespec;
            if (IoUtils::exists(mkspec)) {
                qmakespec = mkspec;
                goto cool;
            }
        }
        evalError(fL1S("Could not find qmake configuration file %1.").arg(qmakespec));
        return false;
    }
  cool:
    m_qmakespec = QDir::cleanPath(qmakespec);

    if (!m_superfile.isEmpty()
        && !evaluateFileDirect(m_superfile, QMakeHandler::EvalConfigFile, LoadProOnly)) {
        return false;
    }
    if (!evaluateFeatureFile(QLatin1String("spec_pre.prf")))
        return false;
    QString spec = m_qmakespec + QLatin1String("/qmake.conf");
    if (!evaluateFileDirect(spec, QMakeHandler::EvalConfigFile, LoadProOnly)) {
        evalError(fL1S("Could not read qmake configuration file %1.").arg(spec));
        return false;
    }
#ifdef Q_OS_UNIX
    m_qmakespecFull = QFileInfo(m_qmakespec).canonicalFilePath();
#else
    // We can't resolve symlinks as they do on Unix, so configure.exe puts
    // the source of the qmake.conf at the end of the default/qmake.conf in
    // the QMAKESPEC_ORIGINAL variable.
    const ProString &orig_spec = first(ProKey("QMAKESPEC_ORIGINAL"));
    m_qmakespecFull = orig_spec.isEmpty() ? m_qmakespec : orig_spec.toQString();
#endif
    valuesRef(ProKey("QMAKESPEC")) << ProString(m_qmakespecFull);
    m_qmakespecName = IoUtils::fileName(m_qmakespecFull).toString();
    if (!evaluateFeatureFile(QLatin1String("spec_post.prf")))
        return false;
    updateFeaturePaths(); // The spec extends the feature search path, so rebuild the cache.
    if (!m_conffile.isEmpty()
        && !evaluateFileDirect(m_conffile, QMakeHandler::EvalConfigFile, LoadProOnly)) {
        return false;
    }
    if (!m_cachefile.isEmpty()
        && !evaluateFileDirect(m_cachefile, QMakeHandler::EvalConfigFile, LoadProOnly)) {
        return false;
    }
    return true;
}

void QMakeEvaluator::setupProject()
{
    setTemplate();
    ProValueMap &vars = m_valuemapStack.top();
    vars[ProKey("TARGET")] << ProString(QFileInfo(currentFileName()).baseName());
    vars[ProKey("_PRO_FILE_")] << ProString(currentFileName());
    vars[ProKey("_PRO_FILE_PWD_")] << ProString(currentDirectory());
    vars[ProKey("OUT_PWD")] << ProString(m_outputDir);
}

void QMakeEvaluator::visitCmdLine(const QString &cmds)
{
    if (!cmds.isEmpty()) {
        if (ProFile *pro = m_parser->parsedProBlock(fL1S("(command line)"), cmds)) {
            if (pro->isOk()) {
                m_locationStack.push(m_current);
                visitProBlock(pro, pro->tokPtr());
                m_current = m_locationStack.pop();
            }
            pro->deref();
        }
    }
}

QMakeEvaluator::VisitReturn QMakeEvaluator::visitProFile(
        ProFile *pro, QMakeHandler::EvalFileType type, LoadFlags flags)
{
    if (!m_cumulative && !pro->isOk())
        return ReturnFalse;

    if (flags & LoadPreFiles) {
        if (!prepareProject(pro->directoryName()))
            return ReturnFalse;

        m_hostBuild = pro->isHostBuild();

#ifdef PROEVALUATOR_THREAD_SAFE
        m_option->mutex.lock();
#endif
        QMakeBaseEnv **baseEnvPtr = &m_option->baseEnvs[QMakeBaseKey(m_buildRoot, m_hostBuild)];
        if (!*baseEnvPtr)
            *baseEnvPtr = new QMakeBaseEnv;
        QMakeBaseEnv *baseEnv = *baseEnvPtr;

#ifdef PROEVALUATOR_THREAD_SAFE
        {
            QMutexLocker locker(&baseEnv->mutex);
            m_option->mutex.unlock();
            if (baseEnv->inProgress) {
                QThreadPool::globalInstance()->releaseThread();
                baseEnv->cond.wait(&baseEnv->mutex);
                QThreadPool::globalInstance()->reserveThread();
                if (!baseEnv->isOk)
                    return ReturnFalse;
            } else
#endif
            if (!baseEnv->evaluator) {
#ifdef PROEVALUATOR_THREAD_SAFE
                baseEnv->inProgress = true;
                locker.unlock();
#endif

                QMakeEvaluator *baseEval = new QMakeEvaluator(m_option, m_parser, m_handler);
                baseEnv->evaluator = baseEval;
                baseEval->m_superfile = m_superfile;
                baseEval->m_conffile = m_conffile;
                baseEval->m_cachefile = m_cachefile;
                baseEval->m_sourceRoot = m_sourceRoot;
                baseEval->m_buildRoot = m_buildRoot;
                baseEval->m_hostBuild = m_hostBuild;
                bool ok = baseEval->loadSpec();

#ifdef PROEVALUATOR_THREAD_SAFE
                locker.relock();
                baseEnv->isOk = ok;
                baseEnv->inProgress = false;
                baseEnv->cond.wakeAll();
#endif

                if (!ok)
                    return ReturnFalse;
            }
#ifdef PROEVALUATOR_THREAD_SAFE
        }
#endif

        initFrom(*baseEnv->evaluator);
    } else {
        if (!m_valuemapInited)
            loadDefaults();
    }

    m_handler->aboutToEval(currentProFile(), pro, type);
    m_profileStack.push(pro);
    valuesRef(ProKey("PWD")) = ProStringList(ProString(currentDirectory()));
    if (flags & LoadPreFiles) {
        setupProject();

        evaluateFeatureFile(QLatin1String("default_pre.prf"));

        visitCmdLine(m_option->precmds);
    }

    visitProBlock(pro, pro->tokPtr());

    if (flags & LoadPostFiles) {
        visitCmdLine(m_option->postcmds);

        evaluateFeatureFile(QLatin1String("default_post.prf"));

        QSet<QString> processed;
        forever {
            bool finished = true;
            ProStringList configs = values(statics.strCONFIG);
            for (int i = configs.size() - 1; i >= 0; --i) {
                QString config = configs.at(i).toQString(m_tmp1).toLower();
                if (!processed.contains(config)) {
                    config.detach();
                    processed.insert(config);
                    if (evaluateFeatureFile(config, true)) {
                        finished = false;
                        break;
                    }
                }
            }
            if (finished)
                break;
        }
    }
    m_profileStack.pop();
    valuesRef(ProKey("PWD")) = ProStringList(ProString(currentDirectory()));
    m_handler->doneWithEval(currentProFile());

    return ReturnTrue;
}


void QMakeEvaluator::updateMkspecPaths()
{
    QStringList ret;
    const QString concat = QLatin1String("/mkspecs");

    foreach (const QString &it, m_option->getPathListEnv(QLatin1String("QMAKEPATH")))
        ret << it + concat;

    foreach (const QString &it, m_qmakepath)
        ret << it + concat;

    if (!m_buildRoot.isEmpty())
        ret << m_buildRoot + concat;
    if (!m_sourceRoot.isEmpty())
        ret << m_sourceRoot + concat;

    ret << m_option->propertyValue(ProKey("QT_HOST_DATA/get")) + concat;

    ret.removeDuplicates();
    m_mkspecPaths = ret;
}

void QMakeEvaluator::updateFeaturePaths()
{
    QString mkspecs_concat = QLatin1String("/mkspecs");
    QString features_concat = QLatin1String("/features/");

    QStringList feature_roots;

    foreach (const QString &f, m_option->getPathListEnv(QLatin1String("QMAKEFEATURES")))
        feature_roots += f;

    feature_roots += m_qmakefeatures;

    feature_roots += m_option->propertyValue(ProKey("QMAKEFEATURES")).toQString(m_mtmp).split(
            m_option->dirlist_sep, QString::SkipEmptyParts);

    QStringList feature_bases;
    if (!m_buildRoot.isEmpty())
        feature_bases << m_buildRoot;
    if (!m_sourceRoot.isEmpty())
        feature_bases << m_sourceRoot;

    foreach (const QString &item, m_option->getPathListEnv(QLatin1String("QMAKEPATH")))
        feature_bases << (item + mkspecs_concat);

    foreach (const QString &item, m_qmakepath)
        feature_bases << (item + mkspecs_concat);

    if (!m_qmakespecFull.isEmpty()) {
        // The spec is already platform-dependent, so no subdirs here.
        feature_roots << (m_qmakespecFull + features_concat);

        // Also check directly under the root directory of the mkspecs collection
        QDir specdir(m_qmakespecFull);
        while (!specdir.isRoot() && specdir.cdUp()) {
            const QString specpath = specdir.path();
            if (specpath.endsWith(mkspecs_concat)) {
                if (IoUtils::exists(specpath + features_concat))
                    feature_bases << specpath;
                break;
            }
        }
    }

    feature_bases << (m_option->propertyValue(ProKey("QT_HOST_DATA/get")).toQString(m_mtmp)
                      + mkspecs_concat);

    foreach (const QString &fb, feature_bases) {
        foreach (const ProString &sfx, values(ProKey("QMAKE_PLATFORM")))
            feature_roots << (fb + features_concat + sfx + QLatin1Char('/'));
        feature_roots << (fb + features_concat);
    }

    for (int i = 0; i < feature_roots.count(); ++i)
        if (!feature_roots.at(i).endsWith((ushort)'/'))
            feature_roots[i].append((ushort)'/');

    feature_roots.removeDuplicates();

    QStringList ret;
    foreach (const QString &root, feature_roots)
        if (IoUtils::exists(root))
            ret << root;
    m_featureRoots = ret;
}

ProString QMakeEvaluator::propertyValue(const ProKey &name) const
{
    if (name == QLatin1String("QMAKE_MKSPECS"))
        return ProString(m_mkspecPaths.join(m_option->dirlist_sep));
    ProString ret = m_option->propertyValue(name);
//    if (ret.isNull())
//        evalError(fL1S("Querying unknown property %1").arg(name.toQString(m_mtmp)));
    return ret;
}

ProFile *QMakeEvaluator::currentProFile() const
{
    if (m_profileStack.count() > 0)
        return m_profileStack.top();
    return 0;
}

QString QMakeEvaluator::currentFileName() const
{
    ProFile *pro = currentProFile();
    if (pro)
        return pro->fileName();
    return QString();
}

QString QMakeEvaluator::currentDirectory() const
{
    ProFile *pro = currentProFile();
    if (pro)
        return pro->directoryName();
    return QString();
}

bool QMakeEvaluator::isActiveConfig(const QString &config, bool regex)
{
    // magic types for easy flipping
    if (config == statics.strtrue)
        return true;
    if (config == statics.strfalse)
        return false;

    if (config == statics.strhost_build)
        return m_hostBuild;

    if (regex && (config.contains(QLatin1Char('*')) || config.contains(QLatin1Char('?')))) {
        QString cfg = config;
        cfg.detach(); // Keep m_tmp out of QRegExp's cache
        QRegExp re(cfg, Qt::CaseSensitive, QRegExp::Wildcard);

        // mkspecs
        if (re.exactMatch(m_qmakespecName))
            return true;

        // CONFIG variable
        int t = 0;
        foreach (const ProString &configValue, values(statics.strCONFIG)) {
            if (re.exactMatch(configValue.toQString(m_tmp[t])))
                return true;
            t ^= 1;
        }
    } else {
        // mkspecs
        if (m_qmakespecName == config)
            return true;

        // CONFIG variable
        if (values(statics.strCONFIG).contains(ProString(config)))
            return true;
    }

    return false;
}

ProStringList QMakeEvaluator::expandVariableReferences(
        const ushort *&tokPtr, int sizeHint, bool joined)
{
    ProStringList ret;
    ret.reserve(sizeHint);
    forever {
        evaluateExpression(tokPtr, &ret, joined);
        switch (*tokPtr) {
        case TokValueTerminator:
        case TokFuncTerminator:
            tokPtr++;
            return ret;
        case TokArgSeparator:
            if (joined) {
                tokPtr++;
                continue;
            }
            // fallthrough
        default:
            Q_ASSERT_X(false, "expandVariableReferences", "Unrecognized token");
            break;
        }
    }
}

QList<ProStringList> QMakeEvaluator::prepareFunctionArgs(const ushort *&tokPtr)
{
    QList<ProStringList> args_list;
    if (*tokPtr != TokFuncTerminator) {
        for (;; tokPtr++) {
            ProStringList arg;
            evaluateExpression(tokPtr, &arg, false);
            args_list << arg;
            if (*tokPtr == TokFuncTerminator)
                break;
            Q_ASSERT(*tokPtr == TokArgSeparator);
        }
    }
    tokPtr++;
    return args_list;
}

ProStringList QMakeEvaluator::evaluateFunction(
        const ProFunctionDef &func, const QList<ProStringList> &argumentsList, bool *ok)
{
    bool oki;
    ProStringList ret;

    if (m_valuemapStack.count() >= 100) {
        evalError(fL1S("Ran into infinite recursion (depth > 100)."));
        oki = false;
    } else {
        m_valuemapStack.push(ProValueMap());
        m_locationStack.push(m_current);
        int loopLevel = m_loopLevel;
        m_loopLevel = 0;

        ProStringList args;
        for (int i = 0; i < argumentsList.count(); ++i) {
            args += argumentsList[i];
            m_valuemapStack.top()[ProKey(QString::number(i+1))] = argumentsList[i];
        }
        m_valuemapStack.top()[statics.strARGS] = args;
        VisitReturn vr = visitProBlock(func.pro(), func.tokPtr());
        oki = (vr != ReturnFalse && vr != ReturnError); // True || Return
        ret = m_returnValue;
        m_returnValue.clear();

        m_loopLevel = loopLevel;
        m_current = m_locationStack.pop();
        m_valuemapStack.pop();
    }
    if (ok)
        *ok = oki;
    if (oki)
        return ret;
    return ProStringList();
}

QMakeEvaluator::VisitReturn QMakeEvaluator::evaluateBoolFunction(
        const ProFunctionDef &func, const QList<ProStringList> &argumentsList,
        const ProString &function)
{
    bool ok;
    ProStringList ret = evaluateFunction(func, argumentsList, &ok);
    if (ok) {
        if (ret.isEmpty())
            return ReturnTrue;
        if (ret.at(0) != statics.strfalse) {
            if (ret.at(0) == statics.strtrue)
                return ReturnTrue;
            int val = ret.at(0).toQString(m_tmp1).toInt(&ok);
            if (ok) {
                if (val)
                    return ReturnTrue;
            } else {
                evalError(fL1S("Unexpected return value from test '%1': %2.")
                          .arg(function.toQString(m_tmp1))
                          .arg(ret.join(QLatin1String(" :: "))));
            }
        }
    }
    return ReturnFalse;
}

bool QMakeEvaluator::evaluateConditional(const QString &cond, const QString &context)
{
    bool ret = false;
    ProFile *pro = m_parser->parsedProBlock(context, cond, QMakeParser::TestGrammar);
    if (pro) {
        if (pro->isOk()) {
            m_locationStack.push(m_current);
            ret = visitProBlock(pro, pro->tokPtr()) == ReturnTrue;
            m_current = m_locationStack.pop();
        }
        pro->deref();
    }
    return ret;
}

#ifdef PROEVALUATOR_FULL
void QMakeEvaluator::checkRequirements(const ProStringList &deps)
{
    ProStringList &failed = valuesRef(ProKey("QMAKE_FAILED_REQUIREMENTS"));
    foreach (const ProString &dep, deps)
        if (!evaluateConditional(dep.toQString(), fL1S("(requires)")))
            failed << dep;
}
#endif

ProValueMap *QMakeEvaluator::findValues(const ProKey &variableName, ProValueMap::Iterator *rit)
{
    for (int i = m_valuemapStack.size(); --i >= 0; ) {
        ProValueMap::Iterator it = m_valuemapStack[i].find(variableName);
        if (it != m_valuemapStack[i].end()) {
            if (it->constBegin() == statics.fakeValue.constBegin())
                return 0;
            *rit = it;
            return &m_valuemapStack[i];
        }
    }
    return 0;
}

ProStringList &QMakeEvaluator::valuesRef(const ProKey &variableName)
{
    ProValueMap::Iterator it = m_valuemapStack.top().find(variableName);
    if (it != m_valuemapStack.top().end()) {
        if (it->constBegin() == statics.fakeValue.constBegin())
            it->clear();
        return *it;
    }
    for (int i = m_valuemapStack.size() - 1; --i >= 0; ) {
        ProValueMap::ConstIterator it = m_valuemapStack.at(i).constFind(variableName);
        if (it != m_valuemapStack.at(i).constEnd()) {
            ProStringList &ret = m_valuemapStack.top()[variableName];
            if (it->constBegin() != statics.fakeValue.constBegin())
                ret = *it;
            return ret;
        }
    }
    return m_valuemapStack.top()[variableName];
}

ProStringList QMakeEvaluator::values(const ProKey &variableName) const
{
    for (int i = m_valuemapStack.size(); --i >= 0; ) {
        ProValueMap::ConstIterator it = m_valuemapStack.at(i).constFind(variableName);
        if (it != m_valuemapStack.at(i).constEnd()) {
            if (it->constBegin() == statics.fakeValue.constBegin())
                break;
            return *it;
        }
    }
    return ProStringList();
}

ProString QMakeEvaluator::first(const ProKey &variableName) const
{
    const ProStringList &vals = values(variableName);
    if (!vals.isEmpty())
        return vals.first();
    return ProString();
}

bool QMakeEvaluator::evaluateFileDirect(
        const QString &fileName, QMakeHandler::EvalFileType type, LoadFlags flags)
{
    if (ProFile *pro = m_parser->parsedProFile(fileName, true)) {
        m_locationStack.push(m_current);
        bool ok = (visitProFile(pro, type, flags) == ReturnTrue);
        m_current = m_locationStack.pop();
        pro->deref();
#ifdef PROEVALUATOR_FULL
        if (ok) {
            ProStringList &iif = m_valuemapStack.first()[ProKey("QMAKE_INTERNAL_INCLUDED_FILES")];
            ProString ifn(fileName);
            if (!iif.contains(ifn))
                iif << ifn;
        }
#endif
        return ok;
    } else {
        if (!(flags & LoadSilent) && IoUtils::exists(fileName))
            languageWarning(fL1S("Include file %1 not found").arg(fileName));
        return false;
    }
}

bool QMakeEvaluator::evaluateFile(
        const QString &fileName, QMakeHandler::EvalFileType type, LoadFlags flags)
{
    if (fileName.isEmpty())
        return false;
    QMakeEvaluator *ref = this;
    do {
        foreach (const ProFile *pf, ref->m_profileStack)
            if (pf->fileName() == fileName) {
                evalError(fL1S("Circular inclusion of %1.").arg(fileName));
                return false;
            }
    } while ((ref = ref->m_caller));
    return evaluateFileDirect(fileName, type, flags);
}

bool QMakeEvaluator::evaluateFeatureFile(const QString &fileName, bool silent)
{
    QString fn = fileName;
    if (!fn.endsWith(QLatin1String(".prf")))
        fn += QLatin1String(".prf");

    if (m_featureRoots.isEmpty())
        updateFeaturePaths();
    int start_root = 0;
    QString currFn = currentFileName();
    if (IoUtils::fileName(currFn) == IoUtils::fileName(fn)) {
        for (int root = 0; root < m_featureRoots.size(); ++root)
            if (currFn == m_featureRoots.at(root) + fn) {
                start_root = root + 1;
                break;
            }
    }
    for (int root = start_root; root < m_featureRoots.size(); ++root) {
        QString fname = m_featureRoots.at(root) + fn;
        if (IoUtils::exists(fname)) {
            fn = fname;
            goto cool;
        }
    }
#ifdef QMAKE_BUILTIN_PRFS
    fn.prepend(QLatin1String(":/qmake/features/"));
    if (QFileInfo(fn).exists())
        goto cool;
#endif
    if (!silent)
        languageWarning(fL1S("Cannot find feature %1").arg(fileName));
    return false;

  cool:
    ProStringList &already = valuesRef(ProKey("QMAKE_INTERNAL_INCLUDED_FEATURES"));
    ProString afn(fn);
    if (already.contains(afn)) {
        if (!silent)
            languageWarning(fL1S("Feature %1 already included").arg(fileName));
        return true;
    }
    already.append(afn);

#ifdef PROEVALUATOR_CUMULATIVE
    bool cumulative = m_cumulative;
    m_cumulative = false;
#endif

    // The path is fully normalized already.
    bool ok = evaluateFileDirect(fn, QMakeHandler::EvalFeatureFile, LoadProOnly);

#ifdef PROEVALUATOR_CUMULATIVE
    m_cumulative = cumulative;
#endif
    return ok;
}

bool QMakeEvaluator::evaluateFileInto(const QString &fileName, QMakeHandler::EvalFileType type,
        ProValueMap *values, LoadFlags flags)
{
    QMakeEvaluator visitor(m_option, m_parser, m_handler);
    visitor.m_caller = this;
    visitor.m_outputDir = m_outputDir;
    visitor.m_featureRoots = m_featureRoots;
    if (!visitor.evaluateFile(fileName, type, flags))
        return false;
    *values = visitor.m_valuemapStack.top();
#ifdef PROEVALUATOR_FULL
    ProKey qiif("QMAKE_INTERNAL_INCLUDED_FILES");
    ProStringList &iif = m_valuemapStack.first()[qiif];
    foreach (const ProString &ifn, values->value(qiif))
        if (!iif.contains(ifn))
            iif << ifn;
#endif
    return true;
}

void QMakeEvaluator::message(int type, const QString &msg) const
{
    if (!m_skipLevel)
        m_handler->message(type, msg,
                m_current.line ? m_current.pro->fileName() : QString(), m_current.line);
}

QT_END_NAMESPACE
