/**********************************************************************
 *
 * PostGIS - Spatial Types for PostgreSQL
 * http://postgis.net
 *
 * Copyright (C) 2015 Sandro Santilli <strk@keybit.net>
 *
 * This is free software; you can redistribute and/or modify it under
 * the terms of the GNU General Public Licence. See the COPYING file.
 *
 **********************************************************************/

#include "postgres.h"
#include "fmgr.h"
#include "utils/elog.h"
#include "utils/memutils.h" /* for TopMemoryContext */
#include "lib/stringinfo.h"
//#include "funcapi.h"
#include "executor/spi.h" /* this is what you need to work with SPI */
#include "inttypes.h" /* for PRId64 */

#include "../postgis_config.h"

#include "liblwgeom_internal.h" /* for gbox_clone */
#include "liblwgeom_topo.h"
/*#define POSTGIS_DEBUG_LEVEL 1*/
#include "lwgeom_log.h"
#include "lwgeom_pg.h"

#include <stdarg.h>

#ifdef __GNUC__
# define GNU_PRINTF23 __attribute__ (( format(printf, 2, 3) ))
#else
# define GNU_PRINTF23
#endif

#define ABS(x) (x<0?-x:x)

/*
 * This is required for builds against pgsql
 */
PG_MODULE_MAGIC;

LWT_BE_IFACE* be_iface;

/*
 * Private data we'll use for this backend
 */
#define MAXERRLEN 256
struct LWT_BE_DATA_T {
  char lastErrorMsg[MAXERRLEN];
  /*
   * This flag will need to be set to false
   * at top-level function enter and set true
   * whenever an callback changes the data
   * in the database.
   * It will be used by SPI_execute calls to
   * make sure to see any data change occurring
   * doring operations.
   */
  bool data_changed;
};

LWT_BE_DATA be_data;

struct LWT_BE_TOPOLOGY_T {
  LWT_BE_DATA* be_data;
  char *name;
  int id;
  int srid;
  int precision;
};

/* utility funx */

static void cberror(const LWT_BE_DATA* be, const char *fmt, ...) GNU_PRINTF23;

static void
cberror(const LWT_BE_DATA* be_in, const char *fmt, ...)
{
  LWT_BE_DATA *be = (LWT_BE_DATA*)be_in;/*const cast*/
	va_list ap;

	va_start(ap, fmt);

	vsnprintf (be->lastErrorMsg, MAXERRLEN, fmt, ap);
	be->lastErrorMsg[MAXERRLEN-1]='\0';

	va_end(ap);
}

/* Backend callbacks */

static const char*
cb_lastErrorMessage(const LWT_BE_DATA* be)
{
  return be->lastErrorMsg;
}

static LWT_BE_TOPOLOGY*
cb_loadTopologyByName(const LWT_BE_DATA* be, const char *name)
{
  int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  Datum dat;
  bool isnull;
  LWT_BE_TOPOLOGY *topo;

  initStringInfo(sql);
  appendStringInfo(sql, "SELECT id,srid FROM topology.topology "
                        "WHERE name = '%s'", name);
  spi_result = SPI_execute(sql->data, !be->data_changed, 0);
  if ( spi_result != SPI_OK_SELECT ) {
    pfree(sqldata.data);
		cberror(be, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  return NULL;
  }
  if ( ! SPI_processed )
  {
    pfree(sqldata.data);
		//cberror(be, "no topology named '%s' was found", name);
		cberror(be, "SQL/MM Spatial exception - invalid topology name");
	  return NULL;
  }
  if ( SPI_processed > 1 )
  {
    pfree(sqldata.data);
		cberror(be, "multiple topologies named '%s' were found", name);
	  return NULL;
  }
  pfree(sqldata.data);

  topo = palloc(sizeof(LWT_BE_TOPOLOGY));
  topo->be_data = (LWT_BE_DATA *)be; /* const cast.. */
  topo->name = pstrdup(name);

  dat = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
  if ( isnull ) {
		cberror(be, "Topology '%s' has null identifier", name);
    return NULL;
  }
  topo->id = DatumGetInt32(dat);

  dat = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
  if ( isnull ) {
		cberror(be, "Topology '%s' has null SRID", name);
    return NULL;
  }
  topo->srid = DatumGetInt32(dat);

  topo->precision = 0; /* needed ? */

  POSTGIS_DEBUGF(1, "cb_loadTopologyByName: topo '%s' has id %d, srid %d",
             name, topo->id, topo->srid);

  return topo;
}

static int
cb_freeTopology(LWT_BE_TOPOLOGY* topo)
{
  pfree(topo->name);
  pfree(topo);
  return 1;
}

static void
addEdgeFields(StringInfo str, int fields, int fullEdgeData)
{
  const char *sep = "";

  if ( fields & LWT_COL_EDGE_EDGE_ID ) {
    appendStringInfoString(str, "edge_id");
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_START_NODE ) {
    appendStringInfo(str, "%sstart_node", sep);
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_END_NODE ) {
    appendStringInfo(str, "%send_node", sep);
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_FACE_LEFT ) {
    appendStringInfo(str, "%sleft_face", sep);
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_FACE_RIGHT ) {
    appendStringInfo(str, "%sright_face", sep);
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_NEXT_LEFT ) {
    appendStringInfo(str, "%snext_left_edge", sep);
    if ( fullEdgeData ) appendStringInfoString(str, ", abs_next_left_edge");
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_NEXT_RIGHT ) {
    appendStringInfo(str, "%snext_right_edge", sep);
    if ( fullEdgeData ) appendStringInfoString(str, ", abs_next_right_edge");
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_GEOM ) {
    appendStringInfo(str, "%sgeom", sep);
  }
}

/* Add edge values in text form, include the parens */
static void
addEdgeValues(StringInfo str, const LWT_ISO_EDGE *edge, int fields, int fullEdgeData)
{
  size_t hexewkb_size;
  char *hexewkb;
  const char *sep = "";

  appendStringInfoChar(str, '(');
  if ( fields & LWT_COL_EDGE_EDGE_ID ) {
    if ( edge->edge_id != -1 )
      appendStringInfo(str, "%" PRId64, edge->edge_id);
    else
      appendStringInfoString(str, "DEFAULT");
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_START_NODE ) {
    appendStringInfo(str, "%s%" PRId64, sep, edge->start_node);
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_END_NODE ) {
    appendStringInfo(str, "%s%" PRId64, sep, edge->end_node);
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_FACE_LEFT ) {
    appendStringInfo(str, "%s%" PRId64, sep, edge->face_left);
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_FACE_RIGHT ) {
    appendStringInfo(str, "%s%" PRId64, sep, edge->face_right);
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_NEXT_LEFT ) {
    appendStringInfo(str, "%s%" PRId64, sep, edge->next_left);
    if ( fullEdgeData )
      appendStringInfo(str, ",%" PRId64, ABS(edge->next_left));
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_NEXT_RIGHT ) {
    appendStringInfo(str, "%s%" PRId64, sep, edge->next_right);
    if ( fullEdgeData )
      appendStringInfo(str, ",%" PRId64, ABS(edge->next_right));
    sep = ",";
  }
  if ( fields & LWT_COL_EDGE_GEOM )
  {
    if ( edge->geom ) {
      hexewkb = lwgeom_to_hexwkb(lwline_as_lwgeom(edge->geom),
                                  WKB_EXTENDED, &hexewkb_size);
      appendStringInfo(str, "%s'%s'::geometry", sep, hexewkb);
      lwfree(hexewkb);
    } else {
      appendStringInfo(str, "%snull", sep);
    }
  }
  appendStringInfoChar(str, ')');
}

enum UpdateType {
  updSet,
  updSel,
  updNot
};

static void
addEdgeUpdate(StringInfo str, const LWT_ISO_EDGE* edge, int fields,
              int fullEdgeData, enum UpdateType updType)
{
  const char *sep = "";
  const char *sep1;
  const char *op;
  size_t hexewkb_size;
  char *hexewkb;

  switch (updType)
  {
    case updSet:
      op = "=";
      sep1 = ",";
      break;
    case updSel:
      op = "=";
      sep1 = " AND ";
      break;
    case updNot:
    default:
      op = "!=";
      sep1 = " AND ";
      break;
  }

  if ( fields & LWT_COL_EDGE_EDGE_ID ) {
    appendStringInfoString(str, "edge_id ");
    appendStringInfo(str, "%s %" PRId64, op, edge->edge_id);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_START_NODE ) {
    appendStringInfo(str, "%sstart_node ", sep);
    appendStringInfo(str, "%s %" PRId64, op, edge->start_node);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_END_NODE ) {
    appendStringInfo(str, "%send_node", sep);
    appendStringInfo(str, "%s %" PRId64, op, edge->end_node);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_FACE_LEFT ) {
    appendStringInfo(str, "%sleft_face", sep);
    appendStringInfo(str, "%s %" PRId64, op, edge->face_left);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_FACE_RIGHT ) {
    appendStringInfo(str, "%sright_face", sep);
    appendStringInfo(str, "%s %" PRId64, op, edge->face_right);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_NEXT_LEFT ) {
    appendStringInfo(str, "%snext_left_edge", sep);
    appendStringInfo(str, "%s %" PRId64, op, edge->next_left);
    sep = sep1;
    if ( fullEdgeData ) {
      appendStringInfo(str, "%s abs_next_left_edge", sep);
      appendStringInfo(str, "%s %" PRId64, op, ABS(edge->next_left));
    }
  }
  if ( fields & LWT_COL_EDGE_NEXT_RIGHT ) {
    appendStringInfo(str, "%snext_right_edge", sep);
    appendStringInfo(str, "%s %" PRId64, op, edge->next_right);
    sep = sep1;
    if ( fullEdgeData ) {
      appendStringInfo(str, "%s abs_next_right_edge", sep);
      appendStringInfo(str, "%s %" PRId64, op, ABS(edge->next_right));
    }
  }
  if ( fields & LWT_COL_EDGE_GEOM ) {
    appendStringInfo(str, "%sgeom", sep);
    hexewkb = lwgeom_to_hexwkb(lwline_as_lwgeom(edge->geom),
                                WKB_EXTENDED, &hexewkb_size);
    appendStringInfo(str, "%s'%s'::geometry", op, hexewkb);
    lwfree(hexewkb);
  }
}

static void
addNodeUpdate(StringInfo str, const LWT_ISO_NODE* node, int fields,
              int fullNodeData, enum UpdateType updType)
{
  const char *sep = "";
  const char *sep1;
  const char *op;
  size_t hexewkb_size;
  char *hexewkb;

  switch (updType)
  {
    case updSet:
      op = "=";
      sep1 = ",";
      break;
    case updSel:
      op = "=";
      sep1 = " AND ";
      break;
    case updNot:
    default:
      op = "!=";
      sep1 = " AND ";
      break;
  }

  if ( fields & LWT_COL_NODE_NODE_ID ) {
    appendStringInfoString(str, "node_id ");
    appendStringInfo(str, "%s %" PRId64, op, node->node_id);
    sep = sep1;
  }
  if ( fields & LWT_COL_NODE_CONTAINING_FACE ) {
    appendStringInfo(str, "%scontaining_face %s", sep, op);
    if ( node->containing_face != -1 ) {
      appendStringInfo(str, "%" PRId64, node->containing_face);
    } else {
      appendStringInfoString(str, "NULL");
    }
    sep = sep1;
  }
  if ( fields & LWT_COL_NODE_GEOM ) {
    appendStringInfo(str, "%sgeom", sep);
    hexewkb = lwgeom_to_hexwkb(lwpoint_as_lwgeom(node->geom),
                                WKB_EXTENDED, &hexewkb_size);
    appendStringInfo(str, "%s'%s'::geometry", op, hexewkb);
    lwfree(hexewkb);
  }
}

static void
addNodeFields(StringInfo str, int fields)
{
  const char *sep = "";

  if ( fields & LWT_COL_NODE_NODE_ID ) {
    appendStringInfoString(str, "node_id");
    sep = ",";
  }
  if ( fields & LWT_COL_NODE_CONTAINING_FACE ) {
    appendStringInfo(str, "%scontaining_face", sep);
    sep = ",";
  }
  if ( fields & LWT_COL_NODE_GEOM ) {
    appendStringInfo(str, "%sgeom", sep);
  }
}

static void
addFaceFields(StringInfo str, int fields)
{
  const char *sep = "";

  if ( fields & LWT_COL_FACE_FACE_ID ) {
    appendStringInfoString(str, "face_id");
    sep = ",";
  }
  if ( fields & LWT_COL_FACE_MBR ) {
    appendStringInfo(str, "%smbr", sep);
    sep = ",";
  }
}

/* Add node values for an insert, in text form */
static void
addNodeValues(StringInfo str, const LWT_ISO_NODE *node, int fields)
{
  size_t hexewkb_size;
  char *hexewkb;
  const char *sep = "";

  appendStringInfoChar(str, '(');

  if ( fields & LWT_COL_NODE_NODE_ID ) {
    if ( node->node_id != -1 )
      appendStringInfo(str, "%" PRId64, node->node_id);
    else
      appendStringInfoString(str, "DEFAULT");
    sep = ",";
  }

  if ( fields & LWT_COL_NODE_CONTAINING_FACE ) {
    if ( node->containing_face != -1 )
      appendStringInfo(str, "%s%" PRId64, sep, node->containing_face);
    else appendStringInfo(str, "%snull", sep);
  }

  if ( fields & LWT_COL_NODE_GEOM ) {
    if ( node->geom ) {
      hexewkb = lwgeom_to_hexwkb(lwpoint_as_lwgeom(node->geom),
                                  WKB_EXTENDED, &hexewkb_size);
      appendStringInfo(str, "%s'%s'::geometry", sep, hexewkb);
      lwfree(hexewkb);
    } else {
      appendStringInfo(str, "%snull", sep);
    }
  }

  appendStringInfoChar(str, ')');
}

/* Add face values for an insert, in text form */
static void
addFaceValues(StringInfo str, LWT_ISO_FACE *face, int srid)
{
  if ( face->face_id != -1 )
    appendStringInfo(str, "(%" PRId64, face->face_id);
  else
    appendStringInfoString(str, "(DEFAULT");

  if ( face->mbr ) {
    appendStringInfo(str, ",ST_SetSRID(ST_MakeEnvelope(%g,%g,%g,%g),%d))",
              face->mbr->xmin, face->mbr->ymin,
              face->mbr->xmax, face->mbr->ymax, srid);
  } else {
    appendStringInfoString(str, ",null)");
  }
}

static void
fillEdgeFields(LWT_ISO_EDGE* edge, HeapTuple row, TupleDesc rowdesc, int fields)
{
  bool isnull;
  Datum dat;
  int val;
  GSERIALIZED *geom;
  int colno = 0;

  POSTGIS_DEBUGF(2, "fillEdgeFields: got %d atts and fields %x",
                    rowdesc->natts, fields);

  if ( fields & LWT_COL_EDGE_EDGE_ID ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( isnull ) {
      lwpgwarning("Found edge with NULL edge_id");
      edge->edge_id = -1;
    }
    val = DatumGetInt32(dat);
    POSTGIS_DEBUGF(2, "fillEdgeFields: colno%d (edge_id)"
                      " has int32 val of %d",
                      colno, val);
    edge->edge_id = val;

  }
  if ( fields & LWT_COL_EDGE_START_NODE ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( isnull ) {
      lwpgwarning("Found edge with NULL start_node");
      edge->start_node = -1;
    }
    val = DatumGetInt32(dat);
    edge->start_node = val;
    POSTGIS_DEBUGF(2, "fillEdgeFields: colno%d (start_node)"
                      " has int32 val of %d", colno, val);
  }
  if ( fields & LWT_COL_EDGE_END_NODE ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( isnull ) {
      lwpgwarning("Found edge with NULL end_node");
      edge->start_node = -1;
    }
    val = DatumGetInt32(dat);
    edge->end_node = val;
    POSTGIS_DEBUGF(2, "fillEdgeFields: colno%d (end_node)"
                      " has int32 val of %d", colno, val);
  }
  if ( fields & LWT_COL_EDGE_FACE_LEFT ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( isnull ) {
      lwpgwarning("Found edge with NULL face_left");
      edge->start_node = -1;
    }
    val = DatumGetInt32(dat);
    edge->face_left = val;
    POSTGIS_DEBUGF(2, "fillEdgeFields: colno%d (face_left)"
                      " has int32 val of %d", colno, val);
  }
#if POSTGIS_DEBUG_LEVEL > 1
  else {
    edge->face_left = 6767; /* debugging */
  }
#endif
  if ( fields & LWT_COL_EDGE_FACE_RIGHT ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( isnull ) {
      lwpgwarning("Found edge with NULL face_right");
      edge->start_node = -1;
    }
    val = DatumGetInt32(dat);
    edge->face_right = val;
    POSTGIS_DEBUGF(2, "fillEdgeFields: colno%d (face_right)"
                      " has int32 val of %d", colno, val);
  }
#if POSTGIS_DEBUG_LEVEL > 1
  else {
    edge->face_right = 6767; /* debugging */
  }
#endif
  if ( fields & LWT_COL_EDGE_NEXT_LEFT ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( isnull ) {
      lwpgwarning("Found edge with NULL next_left");
      edge->start_node = -1;
    }
    val = DatumGetInt32(dat);
    edge->next_left = val;
    POSTGIS_DEBUGF(2, "fillEdgeFields: colno%d (next_left)"
                      " has int32 val of %d", colno, val);
  }
  if ( fields & LWT_COL_EDGE_NEXT_RIGHT ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( isnull ) {
      lwpgwarning("Found edge with NULL next_right");
      edge->start_node = -1;
    }
    val = DatumGetInt32(dat);
    edge->next_right = val;
    POSTGIS_DEBUGF(2, "fillEdgeFields: colno%d (next_right)"
                      " has int32 val of %d", colno, val);
  }
  if ( fields & LWT_COL_EDGE_GEOM ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( ! isnull ) {
      geom = (GSERIALIZED *)PG_DETOAST_DATUM_COPY(dat);
      edge->geom = lwgeom_as_lwline(lwgeom_from_gserialized(geom));
    } else {
      lwpgwarning("Found edge with NULL geometry !");
      edge->geom = NULL;
    }
  }
#if POSTGIS_DEBUG_LEVEL > 1
  else {
    edge->geom = (void*)0x67676767; /* debugging */
  }
#endif
}

static void
fillNodeFields(LWT_ISO_NODE* node, HeapTuple row, TupleDesc rowdesc, int fields)
{
  bool isnull;
  Datum dat;
  GSERIALIZED *geom;
  int colno = 0;

  if ( fields & LWT_COL_NODE_NODE_ID ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    node->node_id = DatumGetInt32(dat);
  }
  if ( fields & LWT_COL_NODE_CONTAINING_FACE ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( isnull ) node->containing_face = -1;
    else node->containing_face = DatumGetInt32(dat);
  }
  if ( fields & LWT_COL_NODE_GEOM ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( ! isnull ) {
      geom = (GSERIALIZED *)PG_DETOAST_DATUM_COPY(dat);
      node->geom = lwgeom_as_lwpoint(lwgeom_from_gserialized(geom));
    } else {
      lwpgnotice("Found node with NULL geometry !");
      node->geom = NULL;
    }
  }
#if POSTGIS_DEBUG_LEVEL > 1
  else {
    node->geom = (void*)0x67676767; /* debugging */
  }
#endif
}

static void
fillFaceFields(LWT_ISO_FACE* face, HeapTuple row, TupleDesc rowdesc, int fields)
{
  bool isnull;
  Datum dat;
  GSERIALIZED *geom;
  LWGEOM *g;
  const GBOX *box;
  int colno = 0;

  if ( fields & LWT_COL_FACE_FACE_ID ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    face->face_id = DatumGetInt32(dat);
  }
  if ( fields & LWT_COL_FACE_MBR ) {
    dat = SPI_getbinval(row, rowdesc, ++colno, &isnull);
    if ( ! isnull ) {
      /* NOTE: this is a geometry of which we want to take (and clone) the BBOX */
      geom = (GSERIALIZED *)PG_DETOAST_DATUM_COPY(dat);
      g = lwgeom_from_gserialized(geom);
      box = lwgeom_get_bbox(g);
      if ( box ) {
        face->mbr = gbox_clone(box);
      } else {
        lwpgnotice("Found face with EMPTY MBR !");
        face->mbr = NULL;
      }
    } else {
      /* NOTE: perfectly fine for universe face */
      POSTGIS_DEBUG(1, "Found face with NULL MBR");
      face->mbr = NULL;
    }
  }
#if POSTGIS_DEBUG_LEVEL > 1
  else {
    face->mbr = (void*)0x67676767; /* debugging */
  }
#endif
}

/* return 0 on failure (null) 1 otherwise */
static int
getNotNullInt32( HeapTuple row, TupleDesc desc, int col, int32 *val )
{
  bool isnull;
  Datum dat = SPI_getbinval( row, desc, col, &isnull );
  if ( isnull ) return 0;
  *val = DatumGetInt32(dat);
  return 1;
}

/* ----------------- Callbacks start here ------------------------ */

static LWT_ISO_EDGE*
cb_getEdgeById(const LWT_BE_TOPOLOGY* topo,
      const LWT_ELEMID* ids, int* numelems, int fields)
{
  LWT_ISO_EDGE *edges;
	int spi_result;

  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;

  initStringInfo(sql);
  appendStringInfoString(sql, "SELECT ");
  addEdgeFields(sql, fields, 0);
  appendStringInfo(sql, " FROM \"%s\".edge_data", topo->name);
  appendStringInfoString(sql, " WHERE edge_id IN (");
  // add all identifiers here
  for (i=0; i<*numelems; ++i) {
    appendStringInfo(sql, "%s%" PRId64, (i?",":""), ids[i]);
  }
  appendStringInfoString(sql, ")");
  POSTGIS_DEBUGF(1, "cb_getEdgeById query: %s", sql->data);

  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, *numelems);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  lwpgnotice("cb_getEdgeById: edge query returned %d rows", SPI_processed);
  *numelems = SPI_processed;
  if ( ! SPI_processed ) {
    return NULL;
  }

  edges = palloc( sizeof(LWT_ISO_EDGE) * SPI_processed );
  for ( i=0; i<SPI_processed; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    fillEdgeFields(&edges[i], row, SPI_tuptable->tupdesc, fields);
  }

  return edges;
}

static LWT_ISO_EDGE*
cb_getEdgeByNode(const LWT_BE_TOPOLOGY* topo,
      const LWT_ELEMID* ids, int* numelems, int fields)
{
  LWT_ISO_EDGE *edges;
	int spi_result;

  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;

  initStringInfo(sql);
  appendStringInfoString(sql, "SELECT ");
  addEdgeFields(sql, fields, 0);
  appendStringInfo(sql, " FROM \"%s\".edge_data", topo->name);
  appendStringInfoString(sql, " WHERE start_node IN (");
  // add all identifiers here
  for (i=0; i<*numelems; ++i) {
    appendStringInfo(sql, "%s%" PRId64, (i?",":""), ids[i]);
  }
  appendStringInfoString(sql, ") OR end_node IN (");
  // add all identifiers here
  for (i=0; i<*numelems; ++i) {
    appendStringInfo(sql, "%s%" PRId64, (i?",":""), ids[i]);
  }
  appendStringInfoString(sql, ")");

  POSTGIS_DEBUGF(1, "cb_getEdgeByNode query: %s", sql->data);
  POSTGIS_DEBUGF(1, "data_changed is %d", topo->be_data->data_changed);

  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, 0);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  POSTGIS_DEBUGF(1, "cb_getEdgeByNode: edge query returned %d rows", SPI_processed);
  *numelems = SPI_processed;
  if ( ! SPI_processed ) {
    return NULL;
  }

  edges = palloc( sizeof(LWT_ISO_EDGE) * SPI_processed );
  for ( i=0; i<SPI_processed; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    fillEdgeFields(&edges[i], row, SPI_tuptable->tupdesc, fields);
  }

  return edges;
}

static LWT_ISO_EDGE*
cb_getEdgeByFace(const LWT_BE_TOPOLOGY* topo,
      const LWT_ELEMID* ids, int* numelems, int fields)
{
  LWT_ISO_EDGE *edges;
	int spi_result;

  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;

  initStringInfo(sql);
  appendStringInfoString(sql, "SELECT ");
  addEdgeFields(sql, fields, 0);
  appendStringInfo(sql, " FROM \"%s\".edge_data", topo->name);
  appendStringInfoString(sql, " WHERE left_face IN (");
  // add all identifiers here
  for (i=0; i<*numelems; ++i) {
    appendStringInfo(sql, "%s%" PRId64, (i?",":""), ids[i]);
  }
  appendStringInfoString(sql, ") OR right_face IN (");
  // add all identifiers here
  for (i=0; i<*numelems; ++i) {
    appendStringInfo(sql, "%s%" PRId64, (i?",":""), ids[i]);
  }
  appendStringInfoString(sql, ")");

  POSTGIS_DEBUGF(1, "cb_getEdgeByFace query: %s", sql->data);
  POSTGIS_DEBUGF(1, "data_changed is %d", topo->be_data->data_changed);

  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, 0);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  POSTGIS_DEBUGF(1, "cb_getEdgeByFace: edge query returned %d rows", SPI_processed);
  *numelems = SPI_processed;
  if ( ! SPI_processed ) {
    return NULL;
  }

  edges = palloc( sizeof(LWT_ISO_EDGE) * SPI_processed );
  for ( i=0; i<SPI_processed; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    fillEdgeFields(&edges[i], row, SPI_tuptable->tupdesc, fields);
  }

  return edges;
}

static LWT_ISO_FACE*
cb_getFacesById(const LWT_BE_TOPOLOGY* topo,
      const LWT_ELEMID* ids, int* numelems, int fields)
{
  LWT_ISO_FACE *faces;
	int spi_result;

  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;

  initStringInfo(sql);
  appendStringInfoString(sql, "SELECT ");
  addFaceFields(sql, fields);
  appendStringInfo(sql, " FROM \"%s\".face", topo->name);
  appendStringInfoString(sql, " WHERE face_id IN (");
  // add all identifiers here
  for (i=0; i<*numelems; ++i) {
    appendStringInfo(sql, "%s%" PRId64, (i?",":""), ids[i]);
  }
  appendStringInfoString(sql, ")");

  POSTGIS_DEBUGF(1, "cb_getFaceById query: %s", sql->data);
  POSTGIS_DEBUGF(1, "data_changed is %d", topo->be_data->data_changed);

  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, 0);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  POSTGIS_DEBUGF(1, "cb_getFaceById: face query returned %d rows", SPI_processed);
  *numelems = SPI_processed;
  if ( ! SPI_processed ) {
    return NULL;
  }

  faces = palloc( sizeof(LWT_ISO_EDGE) * SPI_processed );
  for ( i=0; i<SPI_processed; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    fillFaceFields(&faces[i], row, SPI_tuptable->tupdesc, fields);
  }

  return faces;
}

static LWT_ELEMID*
cb_getRingEdges(const LWT_BE_TOPOLOGY* topo,
      LWT_ELEMID edge, int* numelems, int limit)
{
  LWT_ELEMID *edges;
	int spi_result;
  TupleDesc rowdesc;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;

  initStringInfo(sql);
  appendStringInfo(sql, "WITH RECURSIVE edgering AS ( "
    "SELECT %" PRId64
    " as signed_edge_id, edge_id, next_left_edge, next_right_edge "
    "FROM \"%s\".edge_data WHERE edge_id = %" PRId64 " UNION "
    "SELECT CASE WHEN "
    "p.signed_edge_id < 0 THEN p.next_right_edge ELSE p.next_left_edge END, "
    "e.edge_id, e.next_left_edge, e.next_right_edge "
    "FROM \"%s\".edge_data e, edgering p WHERE "
    "e.edge_id = CASE WHEN p.signed_edge_id < 0 THEN "
    "abs(p.next_right_edge) ELSE abs(p.next_left_edge) END ) "
    "SELECT * FROM edgering",
    edge, topo->name, ABS(edge), topo->name);
  if ( limit ) {
    ++limit; /* so we know if we hit it */
    appendStringInfo(sql, " LIMIT %d", limit);
  }

  POSTGIS_DEBUGF(1, "cb_getRingEdges query (limit %d): %s", limit, sql->data);
  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, limit);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  POSTGIS_DEBUGF(1, "cb_getRingEdges: edge query returned %d rows", SPI_processed);
  *numelems = SPI_processed;
  if ( ! SPI_processed ) {
    return NULL;
  }
  if ( limit && SPI_processed == limit )
  {
    cberror(topo->be_data, "Max traversing limit hit: %d", limit-1);
	  *numelems = -1; return NULL;
  }

  edges = palloc( sizeof(LWT_ELEMID) * SPI_processed );
  rowdesc = SPI_tuptable->tupdesc;
  for ( i=0; i<SPI_processed; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    bool isnull;
    Datum dat;
    int32 val;
    dat = SPI_getbinval(row, rowdesc, 1, &isnull);
    if ( isnull ) {
      lwfree(edges);
      cberror(topo->be_data, "Found edge with NULL edge_id");
      *numelems = -1; return NULL;
    }
    val = DatumGetInt32(dat);
    edges[i] = val;
    POSTGIS_DEBUGF(1, "Component %d in ring of edge %" PRId64 " is edge %d",
                   i, edge, val);
  }

  return edges;
}

static LWT_ISO_NODE*
cb_getNodeById(const LWT_BE_TOPOLOGY* topo,
      const LWT_ELEMID* ids, int* numelems, int fields)
{
  LWT_ISO_NODE *nodes;
	int spi_result;

  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;

  initStringInfo(sql);
  appendStringInfoString(sql, "SELECT ");
  addNodeFields(sql, fields);
  appendStringInfo(sql, " FROM \"%s\".node", topo->name);
  appendStringInfoString(sql, " WHERE node_id IN (");
  // add all identifiers here
  for (i=0; i<*numelems; ++i) {
    appendStringInfo(sql, "%s%" PRId64, (i?",":""), ids[i]);
  }
  appendStringInfoString(sql, ")");
  POSTGIS_DEBUGF(1, "cb_getNodeById query: %s", sql->data);
  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, *numelems);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  lwpgnotice("cb_getNodeById: edge query returned %d rows", SPI_processed);
  *numelems = SPI_processed;
  if ( ! SPI_processed ) {
    return NULL;
  }

  nodes = palloc( sizeof(LWT_ISO_NODE) * SPI_processed );
  for ( i=0; i<SPI_processed; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    fillNodeFields(&nodes[i], row, SPI_tuptable->tupdesc, fields);
  }

  return nodes;
}

static LWT_ISO_NODE*
cb_getNodeByFace(const LWT_BE_TOPOLOGY* topo,
      const LWT_ELEMID* ids, int* numelems, int fields)
{
  LWT_ISO_NODE *nodes;
	int spi_result;

  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;

  initStringInfo(sql);
  appendStringInfoString(sql, "SELECT ");
  addNodeFields(sql, fields);
  appendStringInfo(sql, " FROM \"%s\".node", topo->name);
  appendStringInfoString(sql, " WHERE containing_face IN (");
  // add all identifiers here
  for (i=0; i<*numelems; ++i) {
    appendStringInfo(sql, "%s%" PRId64, (i?",":""), ids[i]);
  }
  appendStringInfoString(sql, ")");
  POSTGIS_DEBUGF(1, "cb_getNodeByFace query: %s", sql->data);
  POSTGIS_DEBUGF(1, "data_changed is %d", topo->be_data->data_changed);
  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, 0);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  lwpgnotice("cb_getNodeByFace: edge query returned %d rows", SPI_processed);
  *numelems = SPI_processed;
  if ( ! SPI_processed ) {
    return NULL;
  }

  nodes = palloc( sizeof(LWT_ISO_NODE) * SPI_processed );
  for ( i=0; i<SPI_processed; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    fillNodeFields(&nodes[i], row, SPI_tuptable->tupdesc, fields);
  }

  return nodes;
}

static LWT_ISO_EDGE*
cb_getEdgeWithinDistance2D(const LWT_BE_TOPOLOGY* topo,
      const LWPOINT* pt, double dist, int* numelems,
      int fields, int limit)
{
  LWT_ISO_EDGE *edges;
	int spi_result;
  int elems_requested = limit;
  size_t hexewkb_size;
  char *hexewkb;

  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;

  initStringInfo(sql);
  if ( elems_requested == -1 ) {
    appendStringInfoString(sql, "SELECT EXISTS ( SELECT 1");
  } else {
    appendStringInfoString(sql, "SELECT ");
    addEdgeFields(sql, fields, 0);
  }
  appendStringInfo(sql, " FROM \"%s\".edge_data", topo->name);
  // TODO: use binary cursor here ?
  hexewkb = lwgeom_to_hexwkb(lwpoint_as_lwgeom(pt), WKB_EXTENDED, &hexewkb_size);
  if ( dist ) {
    appendStringInfo(sql, " WHERE ST_DWithin('%s'::geometry, geom, %g)", hexewkb, dist);
  } else {
    appendStringInfo(sql, " WHERE ST_Within('%s'::geometry, geom)", hexewkb);
  }
  lwfree(hexewkb);
  if ( elems_requested == -1 ) {
    appendStringInfoString(sql, ")");
  } else if ( elems_requested > 0 ) {
    appendStringInfo(sql, " LIMIT %d", elems_requested);
  }
  lwpgnotice("cb_getEdgeWithinDistance2D: query is: %s", sql->data);
  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, limit >= 0 ? limit : 0);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  lwpgnotice("cb_getEdgeWithinDistance2D: edge query "
             "(limited by %d) returned %d rows",
             elems_requested, SPI_processed);
  *numelems = SPI_processed;
  if ( ! SPI_processed ) {
    return NULL;
  }

  if ( elems_requested == -1 )
  {
    /* This was an EXISTS query */
    {
      Datum dat;
      bool isnull, exists;
      dat = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
      exists = DatumGetBool(dat);
      *numelems = exists ? 1 : 0;
      lwpgnotice("cb_getEdgeWithinDistance2D: exists ? %d", *numelems);
    }
    return NULL;
  }

  edges = palloc( sizeof(LWT_ISO_EDGE) * SPI_processed );
  for ( i=0; i<SPI_processed; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    fillEdgeFields(&edges[i], row, SPI_tuptable->tupdesc, fields);
  }

  return edges;
}

static LWT_ISO_NODE*
cb_getNodeWithinDistance2D(const LWT_BE_TOPOLOGY* topo,
      const LWPOINT* pt, double dist, int* numelems,
      int fields, int limit)
{
  LWT_ISO_NODE *nodes;
	int spi_result;
  size_t hexewkb_size;
  char *hexewkb;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int elems_requested = limit;
  int i;

  initStringInfo(sql);
  if ( elems_requested == -1 ) {
    appendStringInfoString(sql, "SELECT EXISTS ( SELECT 1");
  } else {
    appendStringInfoString(sql, "SELECT ");
    if ( fields ) addNodeFields(sql, fields);
    else {
      lwpgwarning("liblwgeom-topo invoked 'getNodeWithinDistance2D' "
                  "backend callback with limit=%d and no fields",
                  elems_requested);
      appendStringInfo(sql, "*");
    }
  }
  appendStringInfo(sql, " FROM \"%s\".node", topo->name);
  // TODO: use binary cursor here ?
  hexewkb = lwgeom_to_hexwkb(lwpoint_as_lwgeom(pt), WKB_EXTENDED, &hexewkb_size);
  if ( dist ) {
    appendStringInfo(sql, " WHERE ST_DWithin(geom, '%s'::geometry, dist)",
                     hexewkb);
  } else {
    appendStringInfo(sql, " WHERE ST_Within(geom, '%s'::geometry)", hexewkb);
  }
  lwfree(hexewkb);
  if ( elems_requested == -1 ) {
    appendStringInfoString(sql, ")");
  } else if ( elems_requested > 0 ) {
    appendStringInfo(sql, " LIMIT %d", elems_requested);
  }
  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, limit >= 0 ? limit : 0);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  lwpgnotice("cb_getNodeWithinDistance2D: node query "
             "(limited by %d) returned %d rows",
             elems_requested, SPI_processed);
  if ( ! SPI_processed ) {
    *numelems = 0; return NULL;
  }

  if ( elems_requested == -1 )
  {
    /* This was an EXISTS query */
    {
      Datum dat;
      bool isnull, exists;
      dat = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
      exists = DatumGetBool(dat);
      *numelems = exists ? 1 : 0;
    }
    return NULL;
  }
  else
  {
    nodes = palloc( sizeof(LWT_ISO_EDGE) * SPI_processed );
    for ( i=0; i<SPI_processed; ++i )
    {
      HeapTuple row = SPI_tuptable->vals[i];
      fillNodeFields(&nodes[i], row, SPI_tuptable->tupdesc, fields);
    }
    *numelems = SPI_processed;
    return nodes;
  }
}

static int
cb_insertNodes( const LWT_BE_TOPOLOGY* topo,
      LWT_ISO_NODE* nodes, int numelems )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;

  initStringInfo(sql);
  appendStringInfo(sql, "INSERT INTO \"%s\".node (", topo->name);
  addNodeFields(sql, LWT_COL_NODE_ALL);
  appendStringInfoString(sql, ") VALUES ");
  for ( i=0; i<numelems; ++i ) {
    if ( i ) appendStringInfoString(sql, ",");
    // TODO: prepare and execute ?
    addNodeValues(sql, &nodes[i], LWT_COL_NODE_ALL);
  }
  appendStringInfoString(sql, " RETURNING node_id");

  POSTGIS_DEBUGF(1, "cb_insertNodes query: %s", sql->data);

  spi_result = SPI_execute(sql->data, false, numelems);
  if ( spi_result != SPI_OK_INSERT_RETURNING ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return 0;
  }
  pfree(sqldata.data);

  if ( SPI_processed ) topo->be_data->data_changed = true;

  if ( SPI_processed != numelems ) {
		cberror(topo->be_data, "processed %d rows, expected %d",
            SPI_processed, numelems);
	  return 0;
  }

  /* Set node_id (could skip this if none had it set to -1) */
  /* TODO: check for -1 values in the first loop */
  for ( i=0; i<SPI_processed; ++i )
  {
    if ( nodes[i].node_id != -1 ) continue;
    fillNodeFields(&nodes[i], SPI_tuptable->vals[i],
      SPI_tuptable->tupdesc, LWT_COL_NODE_NODE_ID);
  }

  return 1;
}

static int
cb_insertEdges( const LWT_BE_TOPOLOGY* topo,
      LWT_ISO_EDGE* edges, int numelems )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;
  int needsEdgeIdReturn = 0;

  initStringInfo(sql);
  /* NOTE: we insert into "edge", on which an insert rule is defined */
  appendStringInfo(sql, "INSERT INTO \"%s\".edge_data (", topo->name);
  addEdgeFields(sql, LWT_COL_EDGE_ALL, 1);
  appendStringInfoString(sql, ") VALUES ");
  for ( i=0; i<numelems; ++i ) {
    if ( i ) appendStringInfoString(sql, ",");
    // TODO: prepare and execute ?
    addEdgeValues(sql, &edges[i], LWT_COL_EDGE_ALL, 1);
    if ( edges[i].edge_id == -1 ) needsEdgeIdReturn = 1;
  }
  if ( needsEdgeIdReturn ) appendStringInfoString(sql, " RETURNING edge_id");

  POSTGIS_DEBUGF(1, "cb_insertEdges query (%d elems): %s", numelems, sql->data);
  spi_result = SPI_execute(sql->data, false, numelems);
  if ( spi_result != ( needsEdgeIdReturn ? SPI_OK_INSERT_RETURNING : SPI_OK_INSERT ) )
  {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -1;
  }
  pfree(sqldata.data);
  if ( SPI_processed ) topo->be_data->data_changed = true;
  POSTGIS_DEBUGF(1, "cb_insertEdges query processed %d rows", SPI_processed);
  if ( SPI_processed != numelems ) {
		cberror(topo->be_data, "processed %d rows, expected %d",
            SPI_processed, numelems);
	  return -1;
  }

  if ( needsEdgeIdReturn )
  {
    /* Set node_id for items that need it */
    for ( i=0; i<SPI_processed; ++i )
    {
      if ( edges[i].edge_id != -1 ) continue;
      fillEdgeFields(&edges[i], SPI_tuptable->vals[i],
        SPI_tuptable->tupdesc, LWT_COL_EDGE_EDGE_ID);
    }
  }

  return SPI_processed;
}

static int
cb_insertFaces( const LWT_BE_TOPOLOGY* topo,
      LWT_ISO_FACE* faces, int numelems )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;
  int needsFaceIdReturn = 0;

  initStringInfo(sql);
  appendStringInfo(sql, "INSERT INTO \"%s\".face (", topo->name);
  addFaceFields(sql, LWT_COL_FACE_ALL);
  appendStringInfoString(sql, ") VALUES ");
  for ( i=0; i<numelems; ++i ) {
    if ( i ) appendStringInfoString(sql, ",");
    // TODO: prepare and execute ?
    addFaceValues(sql, &faces[i], topo->srid);
    if ( faces[i].face_id == -1 ) needsFaceIdReturn = 1;
  }
  if ( needsFaceIdReturn ) appendStringInfoString(sql, " RETURNING face_id");

  POSTGIS_DEBUGF(1, "cb_insertFaces query (%d elems): %s", numelems, sql->data);
  spi_result = SPI_execute(sql->data, false, numelems);
  if ( spi_result != ( needsFaceIdReturn ? SPI_OK_INSERT_RETURNING : SPI_OK_INSERT ) )
  {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -1;
  }
  pfree(sqldata.data);
  if ( SPI_processed ) topo->be_data->data_changed = true;
  POSTGIS_DEBUGF(1, "cb_insertFaces query processed %d rows", SPI_processed);
  if ( SPI_processed != numelems ) {
		cberror(topo->be_data, "processed %d rows, expected %d",
            SPI_processed, numelems);
	  return -1;
  }

  if ( needsFaceIdReturn )
  {
    /* Set node_id for items that need it */
    for ( i=0; i<SPI_processed; ++i )
    {
      if ( faces[i].face_id != -1 ) continue;
      fillFaceFields(&faces[i], SPI_tuptable->vals[i],
        SPI_tuptable->tupdesc, LWT_COL_FACE_FACE_ID);
    }
  }

  return SPI_processed;
}

static int
cb_updateEdges( const LWT_BE_TOPOLOGY* topo,
      const LWT_ISO_EDGE* sel_edge, int sel_fields,
      const LWT_ISO_EDGE* upd_edge, int upd_fields,
      const LWT_ISO_EDGE* exc_edge, int exc_fields )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;

  initStringInfo(sql);
  appendStringInfo(sql, "UPDATE \"%s\".edge_data SET ", topo->name);
  addEdgeUpdate( sql, upd_edge, upd_fields, 1, updSet );
  if ( exc_edge || sel_edge ) appendStringInfoString(sql, " WHERE ");
  if ( sel_edge ) {
    addEdgeUpdate( sql, sel_edge, sel_fields, 1, updSel );
    if ( exc_edge ) appendStringInfoString(sql, " AND ");
  }
  if ( exc_edge ) {
    addEdgeUpdate( sql, exc_edge, exc_fields, 1, updNot );
  }

  POSTGIS_DEBUGF(1, "cb_updateEdges query: %s", sql->data);

  spi_result = SPI_execute( sql->data, false, 0 );
  if ( spi_result != SPI_OK_UPDATE )
  {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -1;
  }
  pfree(sqldata.data);

  if ( SPI_processed ) topo->be_data->data_changed = true;

  lwpgnotice("cb_updateEdges: update query processed %d rows", SPI_processed);

  return SPI_processed;
}

static int
cb_updateNodes( const LWT_BE_TOPOLOGY* topo,
      const LWT_ISO_NODE* sel_node, int sel_fields,
      const LWT_ISO_NODE* upd_node, int upd_fields,
      const LWT_ISO_NODE* exc_node, int exc_fields )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;

  initStringInfo(sql);
  appendStringInfo(sql, "UPDATE \"%s\".node SET ", topo->name);
  addNodeUpdate( sql, upd_node, upd_fields, 1, updSet );
  if ( exc_node || sel_node ) appendStringInfoString(sql, " WHERE ");
  if ( sel_node ) {
    addNodeUpdate( sql, sel_node, sel_fields, 1, updSel );
    if ( exc_node ) appendStringInfoString(sql, " AND ");
  }
  if ( exc_node ) {
    addNodeUpdate( sql, exc_node, exc_fields, 1, updNot );
  }

  POSTGIS_DEBUGF(1, "cb_updateNodes: %s", sql->data);

  spi_result = SPI_execute( sql->data, false, 0 );
  if ( spi_result != SPI_OK_UPDATE )
  {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -1;
  }
  pfree(sqldata.data);

  if ( SPI_processed ) topo->be_data->data_changed = true;

  POSTGIS_DEBUGF(1, "cb_updateNodes: update query processed %d rows", SPI_processed);

  return SPI_processed;
}

static int
cb_updateNodesById( const LWT_BE_TOPOLOGY* topo,
      const LWT_ISO_NODE* nodes, int numnodes, int fields )
{
  int i;
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  const char *sep = "";
  const char *sep1 = ",";

  if ( ! fields ) {
		cberror(topo->be_data,
            "updateNodesById callback called with no update fields!");
	  return -1;
  }

  POSTGIS_DEBUGF(1, "cb_updateNodesById got %d nodes to update"
                    " (fields:%d)",
                    numnodes, fields);

  initStringInfo(sql);
  appendStringInfoString(sql, "WITH newnodes(node_id,");
  addNodeFields(sql, fields);
  appendStringInfoString(sql, ") AS ( VALUES ");
  for (i=0; i<numnodes; ++i) {
    const LWT_ISO_NODE* node = &(nodes[i]);
    if ( i ) appendStringInfoString(sql, ",");
    addNodeValues(sql, node, LWT_COL_NODE_NODE_ID|fields);
  }
  appendStringInfo(sql, " ) UPDATE \"%s\".node n SET ", topo->name);

  /* TODO: turn the following into a function */
  if ( fields & LWT_COL_NODE_NODE_ID ) {
    appendStringInfo(sql, "%snode_id = o.node_id", sep);
    sep = sep1;
  }
  if ( fields & LWT_COL_NODE_CONTAINING_FACE ) {
    appendStringInfo(sql, "%scontaining_face = o.containing_face", sep);
    sep = sep1;
  }
  if ( fields & LWT_COL_NODE_GEOM ) {
    appendStringInfo(sql, "%sgeom = o.geom", sep);
  }

  appendStringInfo(sql, " FROM newnodes o WHERE n.node_id = o.node_id");

  POSTGIS_DEBUGF(1, "cb_updateNodesById query: %s", sql->data);

  spi_result = SPI_execute( sql->data, false, 0 );
  if ( spi_result != SPI_OK_UPDATE )
  {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -1;
  }
  pfree(sqldata.data);

  if ( SPI_processed ) topo->be_data->data_changed = true;

  POSTGIS_DEBUGF(1, "cb_updateNodesById: update query processed %d rows", SPI_processed);

  return SPI_processed;
}

static int
cb_updateFacesById( const LWT_BE_TOPOLOGY* topo,
      const LWT_ISO_FACE* faces, int numfaces )
{
  int i;
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;

  initStringInfo(sql);
  appendStringInfoString(sql, "WITH newfaces AS ( SELECT ");
  for (i=0; i<numfaces; ++i) {
    const LWT_ISO_FACE* face = &(faces[i]);
    appendStringInfo(sql,
      "%" PRId64 " id, ST_SetSRID(ST_MakeEnvelope(%g,%g,%g,%g),%d) mbr",
      face->face_id, face->mbr->xmin, face->mbr->ymin,
      face->mbr->xmax, face->mbr->ymax, topo->srid);
  }
  appendStringInfo(sql, ") UPDATE \"%s\".face o SET mbr = i.mbr "
                        "FROM newfaces i WHERE o.face_id = i.id",
                        topo->name);

  POSTGIS_DEBUGF(1, "cb_updateFacesById query: %s", sql->data);

  spi_result = SPI_execute( sql->data, false, 0 );
  if ( spi_result != SPI_OK_UPDATE )
  {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -1;
  }
  pfree(sqldata.data);

  if ( SPI_processed ) topo->be_data->data_changed = true;

  POSTGIS_DEBUGF(1, "cb_updateFacesById: update query processed %d rows", SPI_processed);

  return SPI_processed;
}

static int
cb_updateEdgesById( const LWT_BE_TOPOLOGY* topo,
      const LWT_ISO_EDGE* edges, int numedges, int fields )
{
  int i;
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  const char *sep = "";
  const char *sep1 = ",";

  if ( ! fields ) {
		cberror(topo->be_data,
            "updateEdgesById callback called with no update fields!");
	  return -1;
  }

  initStringInfo(sql);
  appendStringInfoString(sql, "WITH newedges(edge_id,");
  addEdgeFields(sql, fields, 0);
  appendStringInfoString(sql, ") AS ( VALUES ");
  for (i=0; i<numedges; ++i) {
    const LWT_ISO_EDGE* edge = &(edges[i]);
    if ( i ) appendStringInfoString(sql, ",");
    addEdgeValues(sql, edge, fields|LWT_COL_EDGE_EDGE_ID, 0);
  }
  appendStringInfo(sql, ") UPDATE \"%s\".edge_data e SET ", topo->name);

  /* TODO: turn the following into a function */
  if ( fields & LWT_COL_EDGE_START_NODE ) {
    appendStringInfo(sql, "%sstart_node = o.start_node", sep);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_END_NODE ) {
    appendStringInfo(sql, "%send_node = o.end_node", sep);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_FACE_LEFT ) {
    appendStringInfo(sql, "%sleft_face = o.left_face", sep);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_FACE_RIGHT ) {
    appendStringInfo(sql, "%sright_face = o.right_face", sep);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_NEXT_LEFT ) {
    appendStringInfo(sql,
      "%snext_left_edge = o.next_left_edge, "
      "abs_next_left_edge = abs(o.next_left_edge)", sep);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_NEXT_RIGHT ) {
    appendStringInfo(sql,
      "%snext_right_edge = o.next_right_edge, "
      "abs_next_right_edge = abs(o.next_right_edge)", sep);
    sep = sep1;
  }
  if ( fields & LWT_COL_EDGE_GEOM ) {
    appendStringInfo(sql, "%sgeom = o.geom", sep);
  }

  appendStringInfo(sql, " FROM newedges o WHERE e.edge_id = o.edge_id");

  POSTGIS_DEBUGF(1, "cb_updateEdgesById query: %s", sql->data);

  spi_result = SPI_execute( sql->data, false, 0 );
  if ( spi_result != SPI_OK_UPDATE )
  {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -1;
  }
  pfree(sqldata.data);

  if ( SPI_processed ) topo->be_data->data_changed = true;

  POSTGIS_DEBUGF(1, "cb_updateEdgesById: update query processed %d rows", SPI_processed);

  return SPI_processed;
}

static int
cb_deleteEdges( const LWT_BE_TOPOLOGY* topo,
      const LWT_ISO_EDGE* sel_edge, int sel_fields )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;

  initStringInfo(sql);
  appendStringInfo(sql, "DELETE FROM \"%s\".edge_data WHERE ", topo->name);
  addEdgeUpdate( sql, sel_edge, sel_fields, 0, updSel );

  POSTGIS_DEBUGF(1, "cb_deleteEdges: %s", sql->data);

  spi_result = SPI_execute( sql->data, false, 0 );
  if ( spi_result != SPI_OK_DELETE )
  {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -1;
  }
  pfree(sqldata.data);

  if ( SPI_processed ) topo->be_data->data_changed = true;

  POSTGIS_DEBUGF(1, "cb_deleteEdges: delete query processed %d rows", SPI_processed);

  return SPI_processed;
}

static LWT_ELEMID
cb_getNextEdgeId( const LWT_BE_TOPOLOGY* topo )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  bool isnull;
  Datum dat;
  LWT_ELEMID edge_id;

  initStringInfo(sql);
  appendStringInfo(sql, "SELECT nextval('\"%s\".edge_data_edge_id_seq')",
    topo->name);
  spi_result = SPI_execute(sql->data, false, 0);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -1;
  }
  pfree(sqldata.data);

  if ( SPI_processed ) topo->be_data->data_changed = true;

  if ( SPI_processed != 1 ) {
		cberror(topo->be_data, "processed %d rows, expected 1", SPI_processed);
	  return -1;
  }

  dat = SPI_getbinval( SPI_tuptable->vals[0],
                       SPI_tuptable->tupdesc, 1, &isnull );
  if ( isnull ) {
		cberror(topo->be_data, "nextval for edge_id returned null");
	  return -1;
  }
  edge_id = DatumGetInt64(dat); /* sequences return 64bit integers */
  return edge_id;
}

static int
cb_updateTopoGeomEdgeSplit ( const LWT_BE_TOPOLOGY* topo,
  LWT_ELEMID split_edge, LWT_ELEMID new_edge1, LWT_ELEMID new_edge2 )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i, ntopogeoms;
  const char *proj = "r.element_id, r.topogeo_id, r.layer_id, r.element_type";

  initStringInfo(sql);
  if ( new_edge2 == -1 ) {
    appendStringInfo(sql, "SELECT %s", proj);
  } else {
    appendStringInfoString(sql, "DELETE");
  }
  appendStringInfo( sql, " FROM \"%s\".relation r %s topology.layer l WHERE "
    "l.topology_id = %d AND l.level = 0 AND l.layer_id = r.layer_id "
    "AND abs(r.element_id) = %" PRId64 " AND r.element_type = 2",
    topo->name, (new_edge2 == -1 ? "," : "USING" ), topo->id, split_edge );
  if ( new_edge2 != -1 ) {
    appendStringInfo(sql, " RETURNING %s", proj);
  }

  POSTGIS_DEBUGF(1, "cb_updateTopoGeomEdgeSplit query: %s", sql->data);

  spi_result = SPI_execute(sql->data, new_edge2 == -1 ? !topo->be_data->data_changed : false, 0);
  if ( spi_result != ( new_edge2 == -1 ? SPI_OK_SELECT : SPI_OK_DELETE_RETURNING ) ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return 0;
  }

  if ( spi_result == SPI_OK_DELETE_RETURNING && SPI_processed )
  {
    POSTGIS_DEBUGF(1, "cb_updateTopoGeomEdgeSplit: deleted %d faces", SPI_processed);
    topo->be_data->data_changed = true;
  }

  ntopogeoms = SPI_processed;
  for ( i=0; i<ntopogeoms; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    TupleDesc tdesc = SPI_tuptable->tupdesc;
    int negate;
    int element_id;
    int topogeo_id;
    int layer_id;
    int element_type;

    if ( ! getNotNullInt32( row, tdesc, 1, &element_id ) ) {
		  cberror(topo->be_data,
        "unexpected null element_id in \"%s\".relation",
        topo->name);
	    return 0;
    }
    negate = ( element_id < 0 );

    if ( ! getNotNullInt32( row, tdesc, 2, &topogeo_id ) ) {
		  cberror(topo->be_data,
        "unexpected null topogeo_id in \"%s\".relation",
        topo->name);
	    return 0;
    }

    if ( ! getNotNullInt32( row, tdesc, 3, &layer_id ) ) {
		  cberror(topo->be_data,
        "unexpected null layer_id in \"%s\".relation",
        topo->name);
	    return 0;
    }

    if ( ! getNotNullInt32( row, tdesc, 4, &element_type ) ) {
		  cberror(topo->be_data,
        "unexpected null element_type in \"%s\".relation",
        topo->name);
	    return 0;
    }

    resetStringInfo(sql);
    appendStringInfo(sql,
      "INSERT INTO \"%s\".relation VALUES ("
      "%d,%d,%" PRId64 ",%d)", topo->name,
      topogeo_id, layer_id, negate ? -new_edge1 : new_edge1, element_type);
    spi_result = SPI_execute(sql->data, false, 0);
    if ( spi_result != SPI_OK_INSERT ) {
      cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
              spi_result, sql->data);
      return 0;
    }
    if ( SPI_processed ) topo->be_data->data_changed = true;
    if ( new_edge2 != -1 ) {
      resetStringInfo(sql);
      appendStringInfo(sql,
        "INSERT INTO FROM \"%s\".relation VALUES ("
        "%d,%d,%" PRId64 ",%d", topo->name,
        topogeo_id, layer_id, negate ? -new_edge2 : new_edge2, element_type);
      spi_result = SPI_execute(sql->data, false, 0);
      if ( spi_result != SPI_OK_INSERT ) {
        cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
                spi_result, sql->data);
        return 0;
      }
      if ( SPI_processed ) topo->be_data->data_changed = true;
    }
  }

  POSTGIS_DEBUGF(1, "cb_updateTopoGeomEdgeSplit: updated %d topogeoms", ntopogeoms);

  return 1;
}

static int
cb_updateTopoGeomFaceSplit ( const LWT_BE_TOPOLOGY* topo,
  LWT_ELEMID split_face, LWT_ELEMID new_face1, LWT_ELEMID new_face2 )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i, ntopogeoms;
  const char *proj = "r.element_id, r.topogeo_id, r.layer_id, r.element_type";

  POSTGIS_DEBUGF(1, "cb_updateTopoGeomFaceSplit signalled "
                    "split of face %" PRId64 " into %" PRId64
                    " and %" PRId64,
                    split_face, new_face1, new_face2);

  initStringInfo(sql);
  if ( new_face2 == -1 ) {
    appendStringInfo(sql, "SELECT %s", proj);
  } else {
    appendStringInfoString(sql, "DELETE");
  }
  appendStringInfo( sql, " FROM \"%s\".relation r %s topology.layer l WHERE "
    "l.topology_id = %d AND l.level = 0 AND l.layer_id = r.layer_id "
    "AND abs(r.element_id) = %" PRId64 " AND r.element_type = 3",
    topo->name, (new_face2 == -1 ? "," : "USING" ), topo->id, split_face );
  if ( new_face2 != -1 ) {
    appendStringInfo(sql, " RETURNING %s", proj);
  }

  POSTGIS_DEBUGF(1, "cb_updateTopoGeomFaceSplit query: %s", sql->data);

  spi_result = SPI_execute(sql->data, new_face2 == -1 ? !topo->be_data->data_changed : false, 0);
  if ( spi_result != ( new_face2 == -1 ? SPI_OK_SELECT : SPI_OK_DELETE_RETURNING ) ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return 0;
  }

  if ( spi_result == SPI_OK_DELETE_RETURNING && SPI_processed )
  {
    topo->be_data->data_changed = true;
  }

  ntopogeoms = SPI_processed;
  for ( i=0; i<ntopogeoms; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    TupleDesc tdesc = SPI_tuptable->tupdesc;
    int negate;
    int element_id;
    int topogeo_id;
    int layer_id;
    int element_type;

    if ( ! getNotNullInt32( row, tdesc, 1, &element_id ) ) {
		  cberror(topo->be_data,
        "unexpected null element_id in \"%s\".relation",
        topo->name);
	    return 0;
    }
    negate = ( element_id < 0 );

    if ( ! getNotNullInt32( row, tdesc, 2, &topogeo_id ) ) {
		  cberror(topo->be_data,
        "unexpected null topogeo_id in \"%s\".relation",
        topo->name);
	    return 0;
    }

    if ( ! getNotNullInt32( row, tdesc, 3, &layer_id ) ) {
		  cberror(topo->be_data,
        "unexpected null layer_id in \"%s\".relation",
        topo->name);
	    return 0;
    }

    if ( ! getNotNullInt32( row, tdesc, 4, &element_type ) ) {
		  cberror(topo->be_data,
        "unexpected null element_type in \"%s\".relation",
        topo->name);
	    return 0;
    }

    resetStringInfo(sql);
    appendStringInfo(sql,
      "INSERT INTO \"%s\".relation VALUES ("
      "%d,%d,%" PRId64 ",%d)", topo->name,
      topogeo_id, layer_id, negate ? -new_face1 : new_face1, element_type);

    POSTGIS_DEBUGF(1, "cb_updateTopoGeomFaceSplit query: %s", sql->data);

    spi_result = SPI_execute(sql->data, false, 0);
    if ( spi_result != SPI_OK_INSERT ) {
      cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
              spi_result, sql->data);
      return 0;
    }
    if ( SPI_processed ) topo->be_data->data_changed = true;
    if ( new_face2 != -1 ) {
      resetStringInfo(sql);
      appendStringInfo(sql,
        "INSERT INTO \"%s\".relation VALUES ("
        "%d,%d,%" PRId64 ",%d)", topo->name,
        topogeo_id, layer_id, negate ? -new_face2 : new_face2, element_type);

      POSTGIS_DEBUGF(1, "cb_updateTopoGeomFaceSplit query: %s", sql->data);

      spi_result = SPI_execute(sql->data, false, 0);
      if ( spi_result != SPI_OK_INSERT ) {
        cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
                spi_result, sql->data);
        return 0;
      }
      if ( SPI_processed ) topo->be_data->data_changed = true;
    }
  }

  POSTGIS_DEBUGF(1, "cb_updateTopoGeomFaceSplit: updated %d topogeoms", ntopogeoms);

  return 1;
}

static LWT_ELEMID
cb_getFaceContainingPoint( const LWT_BE_TOPOLOGY* topo, const LWPOINT* pt )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  bool isnull;
  Datum dat;
  LWT_ELEMID face_id;
  size_t hexewkb_size;
  char *hexewkb;

  initStringInfo(sql);

  hexewkb = lwgeom_to_hexwkb(lwpoint_as_lwgeom(pt), WKB_EXTENDED, &hexewkb_size);
  /* TODO: call GetFaceGeometry internally, avoiding the round-trip to sql */
  appendStringInfo(sql, "SELECT face_id FROM \"%s\".face "
                        "WHERE mbr && '%s'::geometry AND ST_Contains("
     "topology.ST_GetFaceGeometry('%s', face_id), "
     "'%s'::geometry) LIMIT 1",
      topo->name, hexewkb, topo->name, hexewkb);
  lwfree(hexewkb);

  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, 1);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -2;
  }
  pfree(sqldata.data);

  if ( SPI_processed != 1 ) {
	  return -1; /* none found */
  }

  dat = SPI_getbinval( SPI_tuptable->vals[0],
                       SPI_tuptable->tupdesc, 1, &isnull );
  if ( isnull ) {
		cberror(topo->be_data, "corrupted topology: face with NULL face_id");
	  return -2;
  }
  face_id = DatumGetInt32(dat);
  return face_id;
}

static int
cb_deleteFacesById( const LWT_BE_TOPOLOGY* topo,
      const LWT_ELEMID* ids, int numelems )
{
	int spi_result, i;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;

  initStringInfo(sql);
  appendStringInfo(sql, "DELETE FROM \"%s\".face WHERE face_id IN (", topo->name);
  for (i=0; i<numelems; ++i) {
    appendStringInfo(sql, "%s%" PRId64, (i?",":""), ids[i]);
  }
  appendStringInfoString(sql, ")");

  POSTGIS_DEBUGF(1, "cb_deleteFacesById query: %s", sql->data);

  spi_result = SPI_execute( sql->data, false, 0 );
  if ( spi_result != SPI_OK_DELETE )
  {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s",
            spi_result, sql->data);
	  return -1;
  }
  pfree(sqldata.data);

  if ( SPI_processed ) topo->be_data->data_changed = true;

  POSTGIS_DEBUGF(1, "cb_deleteFacesById: delete query processed %d rows",
                 SPI_processed);

  return SPI_processed;
}

static LWT_ISO_NODE* 
cb_getNodeWithinBox2D ( const LWT_BE_TOPOLOGY* topo, const GBOX* box,
                     int* numelems, int fields, int limit )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;
  int elems_requested = limit;
  LWT_ISO_NODE* nodes;

  initStringInfo(sql);

  if ( elems_requested == -1 ) {
    appendStringInfoString(sql, "SELECT EXISTS ( SELECT 1");
  } else {
    appendStringInfoString(sql, "SELECT ");
    addNodeFields(sql, fields);
  }
  appendStringInfo(sql, " FROM \"%s\".node WHERE geom && "
                        "ST_SetSRID(ST_MakeEnvelope(%g,%g,%g,%g),%d)",
                        topo->name, box->xmin, box->ymin,
                        box->xmax, box->ymax, topo->srid);
  if ( elems_requested == -1 ) {
    appendStringInfoString(sql, ")");
  } else if ( elems_requested > 0 ) {
    appendStringInfo(sql, " LIMIT %d", elems_requested);
  }
  lwpgnotice("cb_getNodeWithinBox2D: query is: %s", sql->data);
  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, limit >= 0 ? limit : 0);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  lwpgnotice("cb_getNodeWithinBox2D: edge query "
             "(limited by %d) returned %d rows",
             elems_requested, SPI_processed);
  *numelems = SPI_processed;
  if ( ! SPI_processed ) {
    return NULL;
  }

  if ( elems_requested == -1 )
  {
    /* This was an EXISTS query */
    {
      Datum dat;
      bool isnull, exists;
      dat = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
      exists = DatumGetBool(dat);
      *numelems = exists ? 1 : 0;
      lwpgnotice("cb_getNodeWithinBox2D: exists ? %d", *numelems);
    }
    return NULL;
  }

  nodes = palloc( sizeof(LWT_ISO_EDGE) * SPI_processed );
  for ( i=0; i<SPI_processed; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    fillNodeFields(&nodes[i], row, SPI_tuptable->tupdesc, fields);
  }

  return nodes;
}

static LWT_ISO_EDGE* 
cb_getEdgeWithinBox2D ( const LWT_BE_TOPOLOGY* topo, const GBOX* box,
                     int* numelems, int fields, int limit )
{
	int spi_result;
  StringInfoData sqldata;
  StringInfo sql = &sqldata;
  int i;
  int elems_requested = limit;
  LWT_ISO_EDGE* edges;

  initStringInfo(sql);

  if ( elems_requested == -1 ) {
    appendStringInfoString(sql, "SELECT EXISTS ( SELECT 1");
  } else {
    appendStringInfoString(sql, "SELECT ");
    addEdgeFields(sql, fields, 0);
  }
  appendStringInfo(sql, " FROM \"%s\".edge WHERE geom && "
                        "ST_SetSRID(ST_MakeEnvelope(%g,%g,%g,%g),%d)",
                        topo->name, box->xmin, box->ymin,
                        box->xmax, box->ymax, topo->srid);
  if ( elems_requested == -1 ) {
    appendStringInfoString(sql, ")");
  } else if ( elems_requested > 0 ) {
    appendStringInfo(sql, " LIMIT %d", elems_requested);
  }
  lwpgnotice("cb_getEdgeWithinBox2D: query is: %s", sql->data);
  spi_result = SPI_execute(sql->data, !topo->be_data->data_changed, limit >= 0 ? limit : 0);
  if ( spi_result != SPI_OK_SELECT ) {
		cberror(topo->be_data, "unexpected return (%d) from query execution: %s", spi_result, sql->data);
	  *numelems = -1; return NULL;
  }
  pfree(sqldata.data);

  lwpgnotice("cb_getEdgeWithinBox2D: edge query "
             "(limited by %d) returned %d rows",
             elems_requested, SPI_processed);
  *numelems = SPI_processed;
  if ( ! SPI_processed ) {
    return NULL;
  }

  if ( elems_requested == -1 )
  {
    /* This was an EXISTS query */
    {
      Datum dat;
      bool isnull, exists;
      dat = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
      exists = DatumGetBool(dat);
      *numelems = exists ? 1 : 0;
      lwpgnotice("cb_getEdgeWithinBox2D: exists ? %d", *numelems);
    }
    return NULL;
  }

  edges = palloc( sizeof(LWT_ISO_EDGE) * SPI_processed );
  for ( i=0; i<SPI_processed; ++i )
  {
    HeapTuple row = SPI_tuptable->vals[i];
    fillEdgeFields(&edges[i], row, SPI_tuptable->tupdesc, fields);
  }

  return edges;
}


static LWT_BE_CALLBACKS be_callbacks = {
    cb_lastErrorMessage,
    NULL, /* createTopology */
    cb_loadTopologyByName,
    cb_freeTopology,
    cb_getNodeById,
    cb_getNodeWithinDistance2D,
    cb_insertNodes,
    cb_getEdgeById,
    cb_getEdgeWithinDistance2D,
    cb_getNextEdgeId,
    cb_insertEdges,
    cb_updateEdges,
    cb_getFacesById,
    cb_getFaceContainingPoint,
    cb_updateTopoGeomEdgeSplit,
    cb_deleteEdges,
    cb_getNodeWithinBox2D,
    cb_getEdgeWithinBox2D,
    cb_getEdgeByNode,
    cb_updateNodes,
    cb_updateTopoGeomFaceSplit,
    cb_insertFaces,
    cb_updateFacesById,
    cb_getRingEdges,
    cb_updateEdgesById,
    cb_getEdgeByFace,
    cb_getNodeByFace,
    cb_updateNodesById,
    cb_deleteFacesById
};


/*
 * Module load callback
 */
void _PG_init(void);
void
_PG_init(void)
{
  MemoryContext old_context;

  /*
   * install PostgreSQL handlers for liblwgeom
   * NOTE: they may be already in place!
   */
  pg_install_lwgeom_handlers();

  /* Switch to the top memory context so that the backend interface
   * is valid for the whole backend lifetime */
  old_context = MemoryContextSwitchTo( TopMemoryContext );

  /* register callbacks against liblwgeom-topo */
  be_iface = lwt_CreateBackendIface(&be_data);
  lwt_BackendIfaceRegisterCallbacks(be_iface, &be_callbacks);

  /* Switch back to whatever memory context was in place
   * at time of _PG_init enter.
   * See http://www.postgresql.org/message-id/20150623114125.GD5835@localhost
   */
  MemoryContextSwitchTo(old_context);
}

/*
 * Module unload callback
 */
void _PG_fini(void);
void
_PG_fini(void)
{
  elog(NOTICE, "Goodbye from PostGIS Topology %s", POSTGIS_VERSION);
  lwt_FreeBackendIface(be_iface);
}

/*  ST_ModEdgeSplit(atopology, anedge, apoint) */
Datum ST_ModEdgeSplit(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(ST_ModEdgeSplit);
Datum ST_ModEdgeSplit(PG_FUNCTION_ARGS)
{
  text* toponame_text;
  char* toponame;
  LWT_ELEMID edge_id;
  LWT_ELEMID node_id;
  GSERIALIZED *geom;
  LWGEOM *lwgeom;
  LWPOINT *pt;
  LWT_TOPOLOGY *topo;

  if ( PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) ) {
    lwpgerror("SQL/MM Spatial exception - null argument");
    PG_RETURN_NULL();
  }

  toponame_text = PG_GETARG_TEXT_P(0);
  toponame = text2cstring(toponame_text);
	PG_FREE_IF_COPY(toponame_text, 0);

  edge_id = PG_GETARG_INT32(1) ;

  geom = PG_GETARG_GSERIALIZED_P(2);
  lwgeom = lwgeom_from_gserialized(geom);
  pt = lwgeom_as_lwpoint(lwgeom);
  if ( ! pt ) {
    lwgeom_free(lwgeom);
	  PG_FREE_IF_COPY(geom, 2);
    lwpgerror("ST_ModEdgeSplit third argument must be a point geometry");
    PG_RETURN_NULL();
  }

  if ( SPI_OK_CONNECT != SPI_connect() ) {
    lwpgerror("Could not connect to SPI");
    PG_RETURN_NULL();
  }
  be_data.data_changed = false;

  topo = lwt_LoadTopology(be_iface, toponame);
  pfree(toponame);
  if ( ! topo ) {
    /* should never reach this point, as lwerror would raise an exception */
    SPI_finish();
    PG_RETURN_NULL();
  }

  POSTGIS_DEBUG(1, "Calling lwt_ModEdgeSplit");
  node_id = lwt_ModEdgeSplit(topo, edge_id, pt, 0);
  POSTGIS_DEBUG(1, "lwt_ModEdgeSplit returned");
  lwgeom_free(lwgeom);
  PG_FREE_IF_COPY(geom, 3);
  lwt_FreeTopology(topo);

  if ( node_id == -1 ) {
    /* should never reach this point, as lwerror would raise an exception */
    SPI_finish();
    PG_RETURN_NULL();
  }

  SPI_finish();
  PG_RETURN_INT32(node_id);
}

/*  ST_NewEdgesSplit(atopology, anedge, apoint) */
Datum ST_NewEdgesSplit(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(ST_NewEdgesSplit);
Datum ST_NewEdgesSplit(PG_FUNCTION_ARGS)
{
  text* toponame_text;
  char* toponame;
  LWT_ELEMID edge_id;
  LWT_ELEMID node_id;
  GSERIALIZED *geom;
  LWGEOM *lwgeom;
  LWPOINT *pt;
  LWT_TOPOLOGY *topo;

  if ( PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) ) {
    lwpgerror("SQL/MM Spatial exception - null argument");
    PG_RETURN_NULL();
  }

  toponame_text = PG_GETARG_TEXT_P(0);
  toponame = text2cstring(toponame_text);
	PG_FREE_IF_COPY(toponame_text, 0);

  edge_id = PG_GETARG_INT32(1) ;

  geom = PG_GETARG_GSERIALIZED_P(2);
  lwgeom = lwgeom_from_gserialized(geom);
  pt = lwgeom_as_lwpoint(lwgeom);
  if ( ! pt ) {
    lwgeom_free(lwgeom);
	  PG_FREE_IF_COPY(geom, 2);
    lwpgerror("ST_NewEdgesSplit third argument must be a point geometry");
    PG_RETURN_NULL();
  }

  if ( SPI_OK_CONNECT != SPI_connect() ) {
    lwpgerror("Could not connect to SPI");
    PG_RETURN_NULL();
  }
  be_data.data_changed = false;

  topo = lwt_LoadTopology(be_iface, toponame);
  pfree(toponame);
  if ( ! topo ) {
    /* should never reach this point, as lwerror would raise an exception */
    SPI_finish();
    PG_RETURN_NULL();
  }

  POSTGIS_DEBUG(1, "Calling lwt_NewEdgesSplit");
  node_id = lwt_NewEdgesSplit(topo, edge_id, pt, 0);
  POSTGIS_DEBUG(1, "lwt_NewEdgesSplit returned");
  lwgeom_free(lwgeom);
  PG_FREE_IF_COPY(geom, 3);
  lwt_FreeTopology(topo);

  if ( node_id == -1 ) {
    /* should never reach this point, as lwerror would raise an exception */
    SPI_finish();
    PG_RETURN_NULL();
  }

  SPI_finish();
  PG_RETURN_INT32(node_id);
}

/*  ST_AddIsoNode(atopology, aface, apoint) */
Datum ST_AddIsoNode(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(ST_AddIsoNode);
Datum ST_AddIsoNode(PG_FUNCTION_ARGS)
{
  text* toponame_text;
  char* toponame;
  LWT_ELEMID containing_face;
  LWT_ELEMID node_id;
  GSERIALIZED *geom;
  LWGEOM *lwgeom;
  LWPOINT *pt;
  LWT_TOPOLOGY *topo;

  if ( PG_ARGISNULL(0) || PG_ARGISNULL(2) ) {
    lwpgerror("SQL/MM Spatial exception - null argument");
    PG_RETURN_NULL();
  }

  toponame_text = PG_GETARG_TEXT_P(0);
  toponame = text2cstring(toponame_text);
	PG_FREE_IF_COPY(toponame_text, 0);

  if ( PG_ARGISNULL(1) ) containing_face = -1;
  else {
    containing_face = PG_GETARG_INT32(1);
    if ( containing_face < 0 ) {
      lwpgerror("SQL/MM Spatial exception - not within face");
      PG_RETURN_NULL();
    }
  }

  geom = PG_GETARG_GSERIALIZED_P(2);
  lwgeom = lwgeom_from_gserialized(geom);
  pt = lwgeom_as_lwpoint(lwgeom);
  if ( ! pt ) {
    lwgeom_free(lwgeom);
	  PG_FREE_IF_COPY(geom, 2);
#if 0
    lwpgerror("ST_AddIsoNode third argument must be a point geometry");
#else
    lwpgerror("SQL/MM Spatial exception - invalid point");
#endif
    PG_RETURN_NULL();
  }

  if ( SPI_OK_CONNECT != SPI_connect() ) {
    lwpgerror("Could not connect to SPI");
    PG_RETURN_NULL();
  }
  be_data.data_changed = false;

  topo = lwt_LoadTopology(be_iface, toponame);
  pfree(toponame);
  if ( ! topo ) {
    /* should never reach this point, as lwerror would raise an exception */
    SPI_finish();
    PG_RETURN_NULL();
  }

  POSTGIS_DEBUG(1, "Calling lwt_AddIsoNode");
  node_id = lwt_AddIsoNode(topo, containing_face, pt, 0);
  POSTGIS_DEBUG(1, "lwt_AddIsoNode returned");
  lwgeom_free(lwgeom);
  PG_FREE_IF_COPY(geom, 3);
  lwt_FreeTopology(topo);

  if ( node_id == -1 ) {
    /* should never reach this point, as lwerror would raise an exception */
    SPI_finish();
    PG_RETURN_NULL();
  }

  SPI_finish();
  PG_RETURN_INT32(node_id);
}

/*  ST_AddEdgeModFace(atopology, snode, enode, line) */
Datum ST_AddEdgeModFace(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(ST_AddEdgeModFace);
Datum ST_AddEdgeModFace(PG_FUNCTION_ARGS)
{
  text* toponame_text;
  char* toponame;
  LWT_ELEMID startnode_id, endnode_id;
  int edge_id;
  GSERIALIZED *geom;
  LWGEOM *lwgeom;
  LWLINE *line;
  LWT_TOPOLOGY *topo;

  if ( PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3) ) {
    lwpgerror("SQL/MM Spatial exception - null argument");
    PG_RETURN_NULL();
  }

  toponame_text = PG_GETARG_TEXT_P(0);
  toponame = text2cstring(toponame_text);
	PG_FREE_IF_COPY(toponame_text, 0);

  startnode_id = PG_GETARG_INT32(1) ;
  endnode_id = PG_GETARG_INT32(2) ;

  geom = PG_GETARG_GSERIALIZED_P(3);
  lwgeom = lwgeom_from_gserialized(geom);
  line = lwgeom_as_lwline(lwgeom);
  if ( ! line ) {
    lwgeom_free(lwgeom);
	  PG_FREE_IF_COPY(geom, 2);
    lwpgerror("ST_AddEdgeModFace fourth argument must be a line geometry");
    PG_RETURN_NULL();
  }

  if ( SPI_OK_CONNECT != SPI_connect() ) {
    lwpgerror("Could not connect to SPI");
    PG_RETURN_NULL();
  }
  be_data.data_changed = false;

  topo = lwt_LoadTopology(be_iface, toponame);
  pfree(toponame);
  if ( ! topo ) {
    /* should never reach this point, as lwerror would raise an exception */
    SPI_finish();
    PG_RETURN_NULL();
  }

  POSTGIS_DEBUG(1, "Calling lwt_AddEdgeModFace");
  edge_id = lwt_AddEdgeModFace(topo, startnode_id, endnode_id, line, 0);
  POSTGIS_DEBUG(1, "lwt_AddEdgeModFace returned");
  lwgeom_free(lwgeom);
  PG_FREE_IF_COPY(geom, 3);
  lwt_FreeTopology(topo);

  if ( edge_id == -1 ) {
    /* should never reach this point, as lwerror would raise an exception */
    SPI_finish();
    PG_RETURN_NULL();
  }

  SPI_finish();
  PG_RETURN_INT32(edge_id);
}

/*  ST_AddEdgeNewFaces(atopology, snode, enode, line) */
Datum ST_AddEdgeNewFaces(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(ST_AddEdgeNewFaces);
Datum ST_AddEdgeNewFaces(PG_FUNCTION_ARGS)
{
  text* toponame_text;
  char* toponame;
  LWT_ELEMID startnode_id, endnode_id;
  int edge_id;
  GSERIALIZED *geom;
  LWGEOM *lwgeom;
  LWLINE *line;
  LWT_TOPOLOGY *topo;

  if ( PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3) ) {
    lwpgerror("SQL/MM Spatial exception - null argument");
    PG_RETURN_NULL();
  }

  toponame_text = PG_GETARG_TEXT_P(0);
  toponame = text2cstring(toponame_text);
	PG_FREE_IF_COPY(toponame_text, 0);

  startnode_id = PG_GETARG_INT32(1) ;
  endnode_id = PG_GETARG_INT32(2) ;

  geom = PG_GETARG_GSERIALIZED_P(3);
  lwgeom = lwgeom_from_gserialized(geom);
  line = lwgeom_as_lwline(lwgeom);
  if ( ! line ) {
    lwgeom_free(lwgeom);
	  PG_FREE_IF_COPY(geom, 2);
    lwpgerror("ST_AddEdgeModFace fourth argument must be a line geometry");
    PG_RETURN_NULL();
  }

  if ( SPI_OK_CONNECT != SPI_connect() ) {
    lwpgerror("Could not connect to SPI");
    PG_RETURN_NULL();
  }
  be_data.data_changed = false;

  topo = lwt_LoadTopology(be_iface, toponame);
  pfree(toponame);
  if ( ! topo ) {
    /* should never reach this point, as lwerror would raise an exception */
    SPI_finish();
    PG_RETURN_NULL();
  }

  POSTGIS_DEBUG(1, "Calling lwt_AddEdgeNewFaces");
  edge_id = lwt_AddEdgeNewFaces(topo, startnode_id, endnode_id, line, 0);
  POSTGIS_DEBUG(1, "lwt_AddEdgeNewFaces returned");
  lwgeom_free(lwgeom);
  PG_FREE_IF_COPY(geom, 3);
  lwt_FreeTopology(topo);

  if ( edge_id == -1 ) {
    /* should never reach this point, as lwerror would raise an exception */
    SPI_finish();
    PG_RETURN_NULL();
  }

  SPI_finish();
  PG_RETURN_INT32(edge_id);
}
