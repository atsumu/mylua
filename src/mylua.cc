#define MYSQL_DYNAMIC_PLUGIN
#define MYSQL_SERVER 1
#include "mysql_priv.h"

#include "lua.hpp"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

//======================================
// cjson decl

extern "C" {
#define LUA_CJSONLIBNAME "cjson"
int luaopen_cjson(lua_State *l);
}


//======================================
// mylua decl

#define LUA_MYLUALIBNAME "mylua"
int luaopen_mylua(lua_State *lua);


//======================================
// util

void *mylua_xmalloc(size_t size) {
  return malloc(size > 0 ? size : 1);
}


void *mylua_xrealloc(void *oldmem, size_t size) {
  if (size == 0) size = 1;
  return oldmem ? realloc(oldmem, size) : malloc(size);
}


void mylua_xfree(void *p) {
  if (p) {
    free(p);
  }
}


//======================================
// mylua_area

const int MYLUA_KEYBUF_SIZE = 1000; // index prefix length is 1000 bytes at a max.


static const luaL_Reg lualibs[] = {
  {"", luaopen_base},
  {LUA_LOADLIBNAME, luaopen_package},
  {LUA_TABLIBNAME, luaopen_table},
  //{LUA_IOLIBNAME, luaopen_io}, // do not load.
  //{LUA_OSLIBNAME, luaopen_os}, // do not load.
  {LUA_STRLIBNAME, luaopen_string},
  {LUA_MATHLIBNAME, luaopen_math},
  {LUA_DBLIBNAME, luaopen_debug},
  {LUA_CJSONLIBNAME, luaopen_cjson},
  {LUA_MYLUALIBNAME, luaopen_mylua},
#ifdef MYLUA_USE_LUAJIT
  {LUA_BITLIBNAME, luaopen_bit },
  {LUA_JITLIBNAME, luaopen_jit },
#endif
  {NULL, NULL}
};


int mylua_openlibs(lua_State *lua) {
  const luaL_Reg *lib = lualibs;
  for (; lib->func; ++lib) {
    lua_pushcfunction(lua, lib->func);
    lua_pushstring(lua, lib->name);
    lua_call(lua, 1, 0);
  }
  return 0;
}


typedef struct st_mylua_area {
  lua_State *lua;
  lua_Alloc old_allocf;
  void *old_allocd;
  size_t lua_memory_usage;
  size_t lua_memory_limit_bytes;
  uchar *keybuf;
  key_part_map keypart_map;
  uint using_key_parts;
  KEY *key;
  char *result;
  size_t result_size;
  TABLE_LIST *table_list; // for passing between c and lua. allocated on c stack.
  int init_table_done;
  int init_one_table_done;
  int index_init_done;
  int index_read_map_done;
  //FILE *fp; // for debug print
} MYLUA_AREA;


// extend lua (or luajit) default allocate function to limit maximum memory usage.
void *mylua_l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)ud;

  mylua_area->lua_memory_usage += nsize - osize;
  if (mylua_area->lua_memory_usage > mylua_area->lua_memory_limit_bytes) {
    return NULL;
  }

  return mylua_area->old_allocf(mylua_area->old_allocd, ptr, osize, nsize);
}


int mylua_setallocf(lua_State *lua) {
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)lua_touserdata(lua, 1);
  mylua_area->old_allocf = lua_getallocf(lua, &mylua_area->old_allocd);
  lua_setallocf(lua, mylua_l_alloc, mylua_area);
  return 0;
}


void mylua_area_dealloc(MYLUA_AREA *mylua_area) {
  if (mylua_area); else return;
  if (mylua_area->lua) lua_close(mylua_area->lua);
  mylua_xfree(mylua_area->keybuf);
  mylua_xfree(mylua_area->result);
  mylua_xfree(mylua_area);
}


MYLUA_AREA *mylua_area_alloc(uint result_strlen) {
  MYLUA_AREA *mylua_area;

  mylua_area = (MYLUA_AREA *)mylua_xmalloc(sizeof(MYLUA_AREA));
  if (mylua_area); else goto err;
  memset(mylua_area, 0, sizeof(MYLUA_AREA));

  mylua_area->lua_memory_limit_bytes = 1024 * 1024;
  mylua_area->lua_memory_usage = 0;
  mylua_area->lua = luaL_newstate();
  if (mylua_area->lua); else goto err;
  if (lua_cpcall(mylua_area->lua, mylua_setallocf, mylua_area)) goto err;
  if (lua_cpcall(mylua_area->lua, mylua_openlibs, 0)) goto err;

  mylua_area->keybuf = (uchar *)mylua_xmalloc(sizeof(uchar) * MYLUA_KEYBUF_SIZE);
  mylua_area->keypart_map = 0;
  mylua_area->using_key_parts = 0;
  mylua_area->key = NULL;

  mylua_area->result_size = sizeof(char) * result_strlen;
  mylua_area->result = (char *)mylua_xmalloc(mylua_area->result_size);
  if (mylua_area->result); else goto err;
  mylua_area->result[0] = '\0';

  mylua_area->init_table_done = 0;
  mylua_area->init_one_table_done = 0;
  mylua_area->index_init_done = 0;
  mylua_area->index_read_map_done = 0;

  return mylua_area;

err:
  mylua_area_dealloc(mylua_area);
  return 0;
}


int mylua_area_realloc_result(MYLUA_AREA *mylua_area, size_t size) {
  if (size == 0) size = 1;
  char *new_result = (char *)realloc(mylua_area->result, size);
  if (new_result) {
    mylua_area->result = new_result;
    mylua_area->result_size = size;
    return 1;
  } else {
    return 0;
  }
}


//======================================
//

const unsigned long MYLUA_ERRJSON_MAXLEN = 255; // must be >= 40.


enum MYLUA_ARG {
  MYLUA_ARG_PROC,
  MYLUA_ARG_ARG,

  MYLUA_ARG_COUNT,
};


static Item_result mylua_argtype_map[MYLUA_ARG_COUNT] = {
  STRING_RESULT, // MYLUA_ARG_PROC
  STRING_RESULT, // MYLUA_ARG_ARG
};


void mylua_error_json(char *dst, unsigned long *length, const char *msg1, const char *msg2)
{
  const char *json_pre = "{\"ok\":false,\"message\":\"";
  const char *json_suf = "\"}";
  int json_pre_len = strlen(json_pre);
  int json_suf_len = strlen(json_suf);

  int msg_allowed_len = MYLUA_ERRJSON_MAXLEN - json_pre_len - json_suf_len;
  if (msg_allowed_len < 0) {
    // not enough buffer size, then returns empty string.
    dst[0] = '\0';
    *length = 0;
    return;
  }

  memcpy(dst, json_pre, json_pre_len);

  int j = 0;
  const char *msgs[2] = { msg1, msg2 };
  for (int mi = 0; mi < 2; ++mi) {
    int msg_len = strlen(msgs[mi]);
    for (int i = 0; i < msg_len; ++i) {
      // remove '\\' and '"' for prepend invalid json.
      if (msgs[mi][i] == '\\' || msgs[mi][i] == '"') continue;
      if (j >= msg_allowed_len) break;
      dst[json_pre_len + j] = msgs[mi][i];
      ++j;
    }
  }

  memcpy(dst + json_pre_len + j, json_suf, json_suf_len + 1);
  *length = json_pre_len + json_suf_len + j;
}


extern "C" my_bool mylua_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
#define MLI_ASSERT_1(cond, msg) \
  if (cond) { \
  } else { \
    strcpy(message, msg); \
    return 1; \
  }
#define MLI_ASSERT_2(cond, msg) \
  if (cond) { \
  } else { \
    mylua_area_dealloc(tl_area); \
    strcpy(message, msg); \
    return 1; \
  }

  // check arguments.
  MLI_ASSERT_1(args->arg_count == MYLUA_ARG_COUNT, "Wrong arguments count.");
  for (int i = 0; i < MYLUA_ARG_COUNT; ++i) {
    MLI_ASSERT_1(args->args[i], "Not constant argument.");
    MLI_ASSERT_1(args->arg_type[i] == mylua_argtype_map[i], "Wrong argument type.");
  }

  // process for return value.
  // initid->maybe_null = 1; // default is 1.
  //initid->max_length = 65535; // blob
  initid->max_length = 16777215; // medium blob (?)

  // mylua_area
  MYLUA_AREA *mylua_area = mylua_area_alloc(MYLUA_ERRJSON_MAXLEN + 1);
  MLI_ASSERT_1(mylua_area, "Couldn't allocate memory. (mylua_area)");

  initid->ptr = (char *)mylua_area;
  return 0;
}


extern "C" void mylua_deinit(UDF_INIT *initid)
{
  mylua_area_dealloc((MYLUA_AREA *)initid->ptr);
  initid->ptr = 0;
}


typedef struct st_pmylua_arg {
  MYLUA_AREA *mylua_area;
  char *proc;
  char *arg;
  size_t json_len;
  const char *json;
} PMYLUA_ARG;


int pmylua(lua_State *lua) {
  PMYLUA_ARG *pmylua_arg = (PMYLUA_ARG *)lua_touserdata(lua, 1);
  MYLUA_AREA *mylua_area = pmylua_arg->mylua_area;
  char *proc = pmylua_arg->proc;
  char *arg = pmylua_arg->arg;

  //
  lua_pushlightuserdata(lua, mylua_area);
  lua_setfield(lua, LUA_REGISTRYINDEX, "mylua_area");

  // set mylua.arg to json decoded arg.
  lua_getglobal(lua, "mylua");

  lua_getglobal(lua, "cjson");
  lua_getfield(lua, -1, "decode");
  lua_remove(lua, -2);

  lua_pushstring(lua, arg);
  lua_call(lua, 1, 1);
  lua_setfield(lua, -2, "arg");

  // push cjson.encode for encode return value.
  lua_getglobal(lua, "cjson");
  lua_getfield(lua, -1, "encode");
  lua_remove(lua, -2);

  lua_newtable(lua);

  lua_pushstring(lua, "ok");
  lua_pushboolean(lua, 1);
  lua_settable(lua, -3);

  lua_pushstring(lua, "data");
  if (luaL_loadstring(lua, proc)) {
    lua_error(lua);
    return 0;
  }
  lua_call(lua, 0, 1);
  lua_settable(lua, -3);

  // call cjson.encode.
  lua_call(lua, 1, 1);

  pmylua_arg->json = lua_tolstring(lua, -1, &pmylua_arg->json_len);
  return 0;
}


extern "C" char *mylua(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
  *length = 0;
  *is_null = 0;
  *error = 0;

  MYLUA_AREA *mylua_area = (MYLUA_AREA *)initid->ptr;
  lua_State *lua = mylua_area->lua;

  // arguments
  char *proc = args->args[MYLUA_ARG_PROC];
  char *arg  = args->args[MYLUA_ARG_ARG];

  // alloc table_list
  TABLE_LIST table_list;
  mylua_area->table_list = &table_list;

// use macro, because compile error occured when use goto.
#define ML_CLEAN() \
  if (mylua_area->index_init_done) { \
    table_list.table->file->ha_index_end(); \
  } \
  if (mylua_area->init_one_table_done) { \
    close_thread_tables(current_thd); \
  }

#define ML_ASSERT(cond, msg) \
  if (cond) { \
  } else { \
    ML_CLEAN(); \
    const char *errmsg = lua_tostring(lua, -1); \
    if (errmsg == NULL) errmsg = ""; \
    mylua_error_json(mylua_area->result, length, msg, errmsg); \
    if (*length == 0) { \
      *is_null = 1; \
      return 0; \
    } \
    return mylua_area->result; \
  }

  ML_ASSERT(proc, "argument <proc> should not be null.");
  ML_ASSERT(arg, "argument <arg> should not be null.");

  PMYLUA_ARG pmylua_arg = {0};
  pmylua_arg.mylua_area = mylua_area;
  pmylua_arg.proc = proc;
  pmylua_arg.arg = arg;
  if (int err = lua_cpcall(lua, pmylua, &pmylua_arg)) {
    switch (err) {
    case LUA_ERRRUN:
      ML_ASSERT(0, "lua_cpcall(pmylua): LUA_ERRRUN: ");
    case LUA_ERRMEM:
      ML_ASSERT(0, "lua_cpcall(pmylua): LUA_ERRMEM: ");
    case LUA_ERRERR:
      ML_ASSERT(0, "lua_cpcall(pmylua): LUA_ERRERR: ");
    default:
      ML_ASSERT(0, "lua_cpcall(pmylua): defualt: ");
    }
  }

  size_t json_len = pmylua_arg.json_len;
  const char *json = pmylua_arg.json;
  ML_ASSERT(json, "return json is null.");

  if (json_len + 1 > mylua_area->result_size) {
    ML_ASSERT(mylua_area_realloc_result(mylua_area, json_len + 1), "mylua_area_realloc_result failed.");
  }
  memcpy(mylua_area->result, json, json_len + 1);

  ML_CLEAN();

  *length = json_len;
  return mylua_area->result;
}


Field *mylua_get_field(TABLE *table, const char *name) {
  Field **f;
  for (f = table->field; *f != NULL; f++) {
    if (strcmp((*f)->field_name, name) == 0) {
      return *f;
    }
  }
  return NULL;
}


KEY *mylua_index_init(TABLE *table, const char *name, bool sorted) {
  for (uint keynr = 0; keynr < table->s->keynames.count; ++keynr) {
    if (strcmp(table->s->keynames.type_names[keynr], name) == 0) {
      if (table->file->ha_index_init(keynr, sorted)) {
        return NULL;
      }
      return table->key_info + keynr;
    }
  }
  return NULL;
}


//======================================
// mylua API

static int mylua_init_table(lua_State *lua) {
#define MLIT_ASSERT(cond) \
  if (cond) { \
  } else { \
    lua_pushstring(lua, "mylua_init_table: " # cond); \
    lua_error(lua); \
    return 0; \
  }

  int argi = 0;
  int argc = lua_gettop(lua);
  MLIT_ASSERT(argc >= 4);

  while (++argi <= argc) {
    luaL_checktype(lua, argi, LUA_TSTRING);
  }

  argi = 0;

  const char *db = lua_tostring(lua, ++argi);
  const char *tbl = lua_tostring(lua, ++argi);
  const char *idx = lua_tostring(lua, ++argi);
  MLIT_ASSERT(db && tbl && idx);
  int fld_0 = argi;
  uint fld_c = argc - argi;

  lua_getfield(lua, LUA_REGISTRYINDEX, "mylua_area");
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)lua_touserdata(lua, -1);

  TABLE_LIST *table_list = mylua_area->table_list;

  MLIT_ASSERT(!mylua_area->init_one_table_done);
  table_list->init_one_table(db, tbl, TL_READ);
  mylua_area->init_one_table_done = 1;
  MLIT_ASSERT(!simple_open_n_lock_tables(current_thd, table_list));

  TABLE *table = table_list->table;
  MLIT_ASSERT(table);
  table->clear_column_bitmaps();

  while (++argi <= argc) {
    const char *fld = lua_tostring(lua, argi);
    MLIT_ASSERT(fld);
    Field *field = mylua_get_field(table, fld);
    MLIT_ASSERT(field);
    bitmap_set_bit(table->read_set, field->field_index);
  }

  KEY *key = mylua_index_init(table, idx, true);
  MLIT_ASSERT(key);
  mylua_area->index_init_done = 1;
  MLIT_ASSERT(key->key_parts >= fld_c);
  mylua_area->key = key;
  mylua_area->using_key_parts = fld_c;
  mylua_area->keypart_map = (1 << fld_c) - 1;
  memset(mylua_area->keybuf, 0, key->key_length);

  // re-loop, for avoid memory allocation.
  argi = fld_0;
  for (int i = 0; ++argi <= argc; ++i) {
    const char *fld = lua_tostring(lua, argi);
    Field *field = mylua_get_field(table, fld);
    MLIT_ASSERT(strcmp(key->key_part[i].field->field_name, field->field_name) == 0);
  }

  mylua_area->init_table_done = 1;

  return 0;
}


static int mylua_index_read_map(lua_State *lua) {
#define MLIRM_ASSERT(cond) \
  if (cond) { \
  } else { \
    lua_pushstring(lua, "mylua_index_read_map: " # cond); \
    lua_error(lua); \
    return 0; \
  }

  int argi = 0;
  int argc = lua_gettop(lua);
  MLIRM_ASSERT(argc >= 2);
  uint fld_c = argc - 1;

  lua_getfield(lua, LUA_REGISTRYINDEX, "mylua_area");
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)lua_touserdata(lua, -1);
  MLIRM_ASSERT(mylua_area->init_table_done);
  MLIRM_ASSERT(mylua_area->using_key_parts == fld_c);

  TABLE_LIST *table_list = mylua_area->table_list;
  TABLE *table = table_list->table;

  luaL_checktype(lua, ++argi, LUA_TNUMBER);
  ha_rkey_function ha_read_prefix = (ha_rkey_function)lua_tointeger(lua, argi);
  MLIRM_ASSERT(HA_READ_KEY_EXACT <= ha_read_prefix && ha_read_prefix <= HA_READ_MBR_EQUAL);

  size_t offset = 0;
  for (int i = 0; ++argi <= argc; ++i) {
    int type = lua_type(lua, argi);
    MLIRM_ASSERT(type == LUA_TNUMBER);
    longlong ll = lua_tointeger(lua, argi);
    switch (mylua_area->key->key_part[i].type) {
    //case HA_KEYTYPE_TEXT:
    //case HA_KEYTYPE_BINARY:
    //  break;
    case HA_KEYTYPE_SHORT_INT:
    case HA_KEYTYPE_USHORT_INT:
      int2store(mylua_area->keybuf + offset, ll);
      break;
    case HA_KEYTYPE_LONG_INT:
      int4store(mylua_area->keybuf + offset, ll);
      break;
    case HA_KEYTYPE_ULONG_INT:
      int4store(mylua_area->keybuf + offset, ll);
      break;
    case HA_KEYTYPE_LONGLONG:
    case HA_KEYTYPE_ULONGLONG:
      int8store(mylua_area->keybuf + offset, ll);
      break;
    case HA_KEYTYPE_INT24:
    case HA_KEYTYPE_UINT24:
      int3store(mylua_area->keybuf + offset, ll);
      break;
    case HA_KEYTYPE_INT8:
      *((char *)(mylua_area->keybuf + offset)) = ll;
      break;
    default:
      MLIRM_ASSERT(0 && mylua_area->key->key_part[i].type);
      break;
    }
    offset += mylua_area->key->key_part[i].length;
  }

  int error = table->file->index_read_map(table->record[0], mylua_area->keybuf, mylua_area->keypart_map, ha_read_prefix);
  lua_pushinteger(lua, error);

  mylua_area->index_read_map_done = 1;

  return 1;
}


static int mylua_index_prev(lua_State *lua) {
#define MLIP_ASSERT(cond) \
  if (cond) { \
  } else { \
    lua_pushstring(lua, "mylua_index_prev: " # cond); \
    lua_error(lua); \
    return 0; \
  }

  //int argi = 0;
  int argc = lua_gettop(lua);
  MLIP_ASSERT(argc == 0);

  lua_getfield(lua, LUA_REGISTRYINDEX, "mylua_area");
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)lua_touserdata(lua, -1);
  MLIP_ASSERT(mylua_area->index_read_map_done);

  TABLE_LIST *table_list = mylua_area->table_list;
  TABLE *table = table_list->table;

  int error = table->file->index_prev(table->record[0]);

  lua_pushinteger(lua, error);

  return 1;
}


static int mylua_index_next(lua_State *lua) {
#define MLIN_ASSERT(cond) \
  if (cond) { \
  } else { \
    lua_pushstring(lua, "mylua_index_next: " # cond); \
    lua_error(lua); \
    return 0; \
  }

  //int argi = 0;
  int argc = lua_gettop(lua);
  MLIN_ASSERT(argc == 0);

  lua_getfield(lua, LUA_REGISTRYINDEX, "mylua_area");
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)lua_touserdata(lua, -1);
  MLIN_ASSERT(mylua_area->index_read_map_done);

  TABLE_LIST *table_list = mylua_area->table_list;
  TABLE *table = table_list->table;

  int error = table->file->index_next(table->record[0]);

  lua_pushinteger(lua, error);

  return 1;
}


static int mylua_val_int(lua_State *lua) {
#define MLVI_ASSERT(cond) \
  if (cond) { \
  } else { \
    lua_pushstring(lua, "mylua_val_int: " # cond); \
    lua_error(lua); \
    return 0; \
  }

  int argi = 0;
  int argc = lua_gettop(lua);
  MLVI_ASSERT(argc == 1);

  lua_getfield(lua, LUA_REGISTRYINDEX, "mylua_area");
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)lua_touserdata(lua, -1);
  MLVI_ASSERT(mylua_area->index_read_map_done);

  TABLE_LIST *table_list = mylua_area->table_list;
  TABLE *table = table_list->table;

  luaL_checktype(lua, ++argi, LUA_TSTRING);
  const char *fld = lua_tostring(lua, argi);
  Field *field = mylua_get_field(table, fld);
  MLVI_ASSERT(field);

  lua_pushinteger(lua, field->val_int());

  return 1;
}


static int mylua_set_memory_limit_bytes(lua_State *lua) {
#define MLSMLB_ASSERT(cond) \
  if (cond) { \
  } else { \
    lua_pushstring(lua, "mylua_set_memory_limit_bytes: " # cond); \
    lua_error(lua); \
    return 0; \
  }

  int argi = 0;
  int argc = lua_gettop(lua);
  MLSMLB_ASSERT(argc == 1);

  lua_getfield(lua, LUA_REGISTRYINDEX, "mylua_area");
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)lua_touserdata(lua, -1);

  luaL_checktype(lua, ++argi, LUA_TNUMBER);
  mylua_area->lua_memory_limit_bytes = lua_tointeger(lua, argi);

  return 0;
}


static int mylua_get_memory_limit_bytes(lua_State *lua) {
#define MLGMLB_ASSERT(cond) \
  if (cond) { \
  } else { \
    lua_pushstring(lua, "mylua_set_memory_limit_bytes: " # cond); \
    lua_error(lua); \
    return 0; \
  }

  //int argi = 0;
  int argc = lua_gettop(lua);
  MLGMLB_ASSERT(argc == 0);

  lua_getfield(lua, LUA_REGISTRYINDEX, "mylua_area");
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)lua_touserdata(lua, -1);

  lua_pushinteger(lua, mylua_area->lua_memory_limit_bytes);

  return 1;
}


int luaopen_mylua(lua_State *lua) {
  luaL_Reg reg[] = {
    { "init_table", mylua_init_table },
    { "index_read_map", mylua_index_read_map },
    { "index_prev", mylua_index_prev },
    { "index_next", mylua_index_next },
    { "val_int", mylua_val_int },
    { "set_memory_limit_bytes", mylua_set_memory_limit_bytes },
    { "get_memory_limit_bytes", mylua_get_memory_limit_bytes },
    { NULL, NULL }
  };
  luaL_register(lua, LUA_MYLUALIBNAME, reg);

  lua_pushliteral(lua, "0.0.1");
  lua_setfield(lua, -2, "version");

  // set mylua.<name> to value of <name>
#define LUAOPEN_MYLUA_SETCONST(name) \
  lua_pushinteger(lua, name); \
  lua_setfield(lua, -2, # name);

  // first argument of mylua.index_read_map
  LUAOPEN_MYLUA_SETCONST(HA_READ_KEY_EXACT);
  LUAOPEN_MYLUA_SETCONST(HA_READ_KEY_OR_NEXT);
  LUAOPEN_MYLUA_SETCONST(HA_READ_KEY_OR_PREV);
  LUAOPEN_MYLUA_SETCONST(HA_READ_AFTER_KEY);
  LUAOPEN_MYLUA_SETCONST(HA_READ_BEFORE_KEY);
  LUAOPEN_MYLUA_SETCONST(HA_READ_PREFIX);
  LUAOPEN_MYLUA_SETCONST(HA_READ_PREFIX_LAST);
  LUAOPEN_MYLUA_SETCONST(HA_READ_PREFIX_LAST_OR_PREV);
  LUAOPEN_MYLUA_SETCONST(HA_READ_MBR_CONTAIN);
  LUAOPEN_MYLUA_SETCONST(HA_READ_MBR_INTERSECT);
  LUAOPEN_MYLUA_SETCONST(HA_READ_MBR_WITHIN);
  LUAOPEN_MYLUA_SETCONST(HA_READ_MBR_DISJOINT);
  LUAOPEN_MYLUA_SETCONST(HA_READ_MBR_EQUAL);

  // return value of mylua.index_read_map
  LUAOPEN_MYLUA_SETCONST(HA_ERR_KEY_NOT_FOUND);

  // return value of mylua.index_prev and mylua.index_next
  LUAOPEN_MYLUA_SETCONST(HA_ERR_END_OF_FILE);

  // return mylua table.
  return 1;
}

