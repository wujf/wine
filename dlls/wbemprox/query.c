/*
 * Copyright 2012 Hans Leidekker for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include "config.h"
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wbemcli.h"

#include "wine/debug.h"
#include "wbemprox_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

HRESULT create_view( const struct property *proplist, const WCHAR *class,
                     const struct expr *cond, struct view **ret )
{
    struct view *view = heap_alloc( sizeof(struct view) );

    if (!view) return E_OUTOFMEMORY;
    view->proplist = proplist;
    view->table    = get_table( class );
    view->cond     = cond;
    view->result   = NULL;
    view->count    = 0;
    view->index    = 0;
    *ret = view;
    return S_OK;
}

void destroy_view( struct view *view )
{
    heap_free( view->result );
    heap_free( view );
}

static HRESULT get_column_index( const struct table *table, const WCHAR *name, UINT *column )
{
    UINT i;
    for (i = 0; i < table->num_cols; i++)
    {
        if (!strcmpiW( table->columns[i].name, name ))
        {
            *column = i;
            return S_OK;
        }
    }
    return WBEM_E_INVALID_QUERY;
}

static UINT get_column_size( const struct table *table, UINT column )
{
    if (table->columns[column].type & CIM_FLAG_ARRAY) return sizeof(void *);

    switch (table->columns[column].type)
    {
    case CIM_SINT16:
    case CIM_UINT16:
        return sizeof(INT16);
    case CIM_SINT32:
    case CIM_UINT32:
        return sizeof(INT32);
    case CIM_DATETIME:
    case CIM_STRING:
        return sizeof(WCHAR *);
    default:
        ERR("unkown column type %u\n", table->columns[column].type);
        break;
    }
    return sizeof(INT32);
}

static UINT get_column_offset( const struct table *table, UINT column )
{
    UINT i, offset = 0;
    for (i = 0; i < column; i++) offset += get_column_size( table, i );
    return offset;
}

static UINT get_row_size( const struct table *table )
{
    return get_column_offset( table, table->num_cols - 1 ) + get_column_size( table, table->num_cols - 1 );
}

static HRESULT get_value( const struct table *table, UINT row, UINT column, INT_PTR *val )
{
    UINT col_offset, row_size;
    const BYTE *ptr;

    col_offset = get_column_offset( table, column );
    row_size = get_row_size( table );
    ptr = table->data + row * row_size + col_offset;

    if (table->columns[column].type & CIM_FLAG_ARRAY)
    {
        *val = (INT_PTR)*(const void **)ptr;
        return S_OK;
    }
    switch (table->columns[column].type)
    {
    case CIM_DATETIME:
    case CIM_STRING:
        *val = (INT_PTR)*(const WCHAR **)ptr;
        break;
    case CIM_SINT16:
        *val = *(const INT16 *)ptr;
        break;
    case CIM_UINT16:
        *val = *(const UINT16 *)ptr;
        break;
    case CIM_SINT32:
        *val = *(const INT32 *)ptr;
        break;
    case CIM_UINT32:
        *val = *(const UINT32 *)ptr;
        break;
    default:
        ERR("invalid column type %u\n", table->columns[column].type);
        *val = 0;
        break;
    }
    return S_OK;
}

static BOOL eval_like( INT_PTR lval, INT_PTR rval )
{
    const WCHAR *p = (const WCHAR *)lval, *q = (const WCHAR *)rval;

    while (*p && *q)
    {
        if (*q == '%')
        {
            while (*q == '%') q++;
            if (!*q) return TRUE;
            while (*p && toupperW( p[1] ) != toupperW( q[1] )) p++;
            if (!*p) return TRUE;
        }
        if (toupperW( *p++ ) != toupperW( *q++ )) return FALSE;
    }
    return TRUE;
}

static HRESULT eval_cond( const struct table *, UINT, const struct expr *, INT_PTR * );

static BOOL eval_binary( const struct table *table, UINT row, const struct complex_expr *expr,
                         INT_PTR *val )
{
    HRESULT lret, rret;
    INT_PTR lval, rval;

    lret = eval_cond( table, row, expr->left, &lval );
    rret = eval_cond( table, row, expr->right, &rval );
    if (lret != S_OK || rret != S_OK) return WBEM_E_INVALID_QUERY;

    switch (expr->op)
    {
    case OP_EQ:
        *val = (lval == rval);
        break;
    case OP_AND:
        *val = (lval && rval);
        break;
    case OP_OR:
        *val = (lval || rval);
        break;
    case OP_GT:
        *val = (lval > rval);
        break;
    case OP_LT:
        *val = (lval < rval);
        break;
    case OP_LE:
        *val = (lval <= rval);
        break;
    case OP_GE:
        *val = (lval >= rval);
        break;
    case OP_NE:
        *val = (lval != rval);
        break;
    case OP_LIKE:
        *val = eval_like( lval, rval );
        break;
    default:
        ERR("unknown operator %u\n", expr->op);
        return WBEM_E_INVALID_QUERY;
    }
    return S_OK;
}

static HRESULT eval_unary( const struct table *table, UINT row, const struct complex_expr *expr,
                           INT_PTR *val )

{
    HRESULT hr;
    UINT column;
    INT_PTR lval;

    hr = get_column_index( table, expr->left->u.propval->name, &column );
    if (hr != S_OK)
        return hr;

    hr = get_value( table, row, column, &lval );
    if (hr != S_OK)
        return hr;

    switch (expr->op)
    {
    case OP_ISNULL:
        *val = !lval;
        break;
    case OP_NOTNULL:
        *val = lval;
        break;
    default:
        ERR("unknown operator %u\n", expr->op);
        return WBEM_E_INVALID_QUERY;
    }
    return S_OK;
}

static HRESULT eval_propval( const struct table *table, UINT row, const struct property *propval,
                             INT_PTR *val )

{
    HRESULT hr;
    UINT column;

    hr = get_column_index( table, propval->name, &column );
    if (hr != S_OK)
        return hr;

    return get_value( table, row, column, val );
}

static HRESULT eval_cond( const struct table *table, UINT row, const struct expr *cond,
                          INT_PTR *val )
{
    if (!cond)
    {
        *val = 1;
        return S_OK;
    }
    switch (cond->type)
    {
    case EXPR_COMPLEX:
        return eval_binary( table, row, &cond->u.expr, val );
    case EXPR_UNARY:
        return eval_unary( table, row, &cond->u.expr, val );
    case EXPR_PROPVAL:
        return eval_propval( table, row, cond->u.propval, val );
    case EXPR_SVAL:
        *val = (INT_PTR)cond->u.sval;
        return S_OK;
    case EXPR_IVAL:
    case EXPR_BVAL:
        *val = cond->u.ival;
        return S_OK;
    default:
        ERR("invalid expression type\n");
        break;
    }
    return WBEM_E_INVALID_QUERY;
}

static HRESULT execute_view( struct view *view )
{
    UINT i, j = 0, len;

    if (!view->table || !view->table->num_rows) return S_OK;

    len = min( view->table->num_rows, 16 );
    if (!(view->result = heap_alloc( len * sizeof(UINT) ))) return E_OUTOFMEMORY;

    for (i = 0; i < view->table->num_rows; i++)
    {
        HRESULT hr;
        INT_PTR val = 0;

        if (j >= len)
        {
            UINT *tmp;
            len *= 2;
            if (!(tmp = heap_realloc( view->result, len * sizeof(UINT) ))) return E_OUTOFMEMORY;
            view->result = tmp;
        }
        if ((hr = eval_cond( view->table, i, view->cond, &val )) != S_OK) return hr;
        if (val) view->result[j++] = i;
    }
    view->count = j;
    return S_OK;
}

static struct query *alloc_query(void)
{
    struct query *query;

    if (!(query = heap_alloc( sizeof(*query) ))) return NULL;
    list_init( &query->mem );
    return query;
}

void free_query( struct query *query )
{
    struct list *mem, *next;

    destroy_view( query->view );
    LIST_FOR_EACH_SAFE( mem, next, &query->mem )
    {
        heap_free( mem );
    }
    heap_free( query );
}

HRESULT exec_query( const WCHAR *str, IEnumWbemClassObject **result )
{
    HRESULT hr;
    struct query *query;

    *result = NULL;
    if (!(query = alloc_query())) return E_OUTOFMEMORY;
    hr = parse_query( str, &query->view, &query->mem );
    if (hr != S_OK) goto done;
    hr = execute_view( query->view );
    if (hr != S_OK) goto done;
    hr = EnumWbemClassObject_create( NULL, query, (void **)result );

done:
    if (hr != S_OK) free_query( query );
    return hr;
}

static BOOL is_selected_prop( const struct view *view, const WCHAR *name )
{
    const struct property *prop = view->proplist;

    if (!prop) return TRUE;
    while (prop)
    {
        if (!strcmpiW( prop->name, name )) return TRUE;
        prop = prop->next;
    }
    return FALSE;
}

HRESULT get_propval( const struct view *view, UINT index, const WCHAR *name, VARIANT *ret, CIMTYPE *type )
{
    HRESULT hr;
    UINT column, row = view->result[index];
    INT_PTR val;

    if (!is_selected_prop( view, name )) return WBEM_E_NOT_FOUND;

    hr = get_column_index( view->table, name, &column );
    if (hr != S_OK) return WBEM_E_NOT_FOUND;

    hr = get_value( view->table, row, column, &val );
    if (hr != S_OK) return hr;

    switch (view->table->columns[column].type)
    {
    case CIM_STRING:
    case CIM_DATETIME:
        V_VT( ret ) = VT_BSTR;
        V_BSTR( ret ) = SysAllocString( (const WCHAR *)val );
        break;
    default:
        ERR("unhandled column type %u\n", view->table->columns[column].type);
        return WBEM_E_FAILED;
    }
    if (type) *type = view->table->columns[column].type;
    return S_OK;
}