#define MYSQL_DYNAMIC_PLUGIN
#define MYSQL_SERVER 1
#include "mysql_priv.h"

// g++ -I ./lua/include/ -L ./lua/lib/ -lm -ldl -Wall -nostartfiles -fPIC -I ./mysql-5.1.41/include -I ./mysql-5.1.41/sql -I ./mysql-5.1.41/regex -shared -o mylua.so mylua.cc lua/lib/liblua.a lua-cjson-1.0.4/cjson.a
// g++ -I ./lua/include/ -L /usr/lib -lm -ldl -Wall -nostartfiles -fPIC -I ./mysql-5.1.41/include -I ./mysql-5.1.41/sql -I ./mysql-5.1.41/regex -shared -o mylua.so mylua.cc lua-cjson-1.0.4/cjson.a -l lua5.1
#include "lua/include/lua.hpp"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// enum_field_types Item::string_field_type() const
// {
//   enum_field_types f_type= MYSQL_TYPE_VAR_STRING;
//   if (max_length >= 16777216)
//     f_type= MYSQL_TYPE_LONG_BLOB;
//   else if (max_length >= 65536)
//     f_type= MYSQL_TYPE_MEDIUM_BLOB;
//   return f_type;
// }

//======================================
// cjson decl
extern "C" {
int luaopen_cjson(lua_State *l);
}

//======================================
// mylua decl
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

const int MYLUA_KEYBUF_SIZE = 1000; // インデックスのプレフィックス長は最大1000バイト

typedef struct st_mylua_area {
  lua_State *lua;
  uchar *keybuf;
  key_part_map keypart_map;
  KEY *key;
  char *result;
  size_t result_size;
  TABLE_LIST *table_list; // lua受け渡し用。スタックにアロケートされる
  int init_table_done;
  int init_one_table_done;
  int index_init_done;
  int index_read_map_done;
  //FILE *fp; // for debug print
} MYLUA_AREA;

void mylua_area_dealloc(MYLUA_AREA *mylua_area) {
  if (mylua_area); else return;
  if (mylua_area->lua) lua_close(mylua_area->lua);
  mylua_xfree(mylua_area->keybuf);
  mylua_xfree(mylua_area->result);
  mylua_xfree(mylua_area);
}

MYLUA_AREA *mylua_area_alloc(uint result_strlen) {
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)mylua_xmalloc(sizeof(MYLUA_AREA));
  if (mylua_area); else goto err;
  memset(mylua_area, 0, sizeof(MYLUA_AREA));

  mylua_area->lua = luaL_newstate();
  if (mylua_area->lua); else goto err;
  luaL_openlibs(mylua_area->lua);
  luaopen_cjson(mylua_area->lua);
  luaopen_mylua(mylua_area->lua);

  mylua_area->keybuf = (uchar *)mylua_xmalloc(sizeof(uchar) * MYLUA_KEYBUF_SIZE);
  mylua_area->keypart_map = 0;
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

const unsigned long MYLUA_ERR_MAX = 255;

enum MYLUA_ARG {
  MYLUA_ARG_PROC,
  MYLUA_ARG_ARG,

  MYLUA_ARG_COUNT,
};

//// TODO: テーブルからselectしてきた各行の値を渡せるバージョン
//enum MYLUA_ARG {
//  MYLUA_ARG_INITPROC,
//  MYLUA_ARG_MAINPROC,
//  MYLUA_ARG_ARGC,
//  // ... argv
//};

static Item_result mylua_argtype_map[MYLUA_ARG_COUNT] = {
  STRING_RESULT, // MYLUA_ARG_PROC
  STRING_RESULT, // MYLUA_ARG_ARG
};

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

  // 引数の処理
  MLI_ASSERT_1(args->arg_count == MYLUA_ARG_COUNT, "Wrong arguments count.");

  for (int i = 0; i < MYLUA_ARG_COUNT; ++i) {
    MLI_ASSERT_1(args->arg_type[i] == mylua_argtype_map[i], "Wrong argument type.");
  }

  // 返値のための処理
  // initid->max_lengthの65535は返値がBLOBだということを表すマジックナンバー。
  // かと思ったら、マジックナンバーじゃないかも？ソースコードをちら見した感じだと、min(initid->max_length, BLOB_WIDTH_MAX)してるだけ。(BLOB_WIDTH_MAX==16777216)
  // メモリが自動で割り当てられないので、自分で割り当てる必要がある。
  // TODO: 実際に返せる文字数を実行してみて確認。
  // initid->maybe_null = 1; // default is 1.
  //initid->max_length = 65535; // blob
  initid->max_length = 16777215; // medium blob (?)

  // mylua_area
  // 最低限エラーメッセージが格納できる容量を確保する。
  MYLUA_AREA *mylua_area = mylua_area_alloc(MYLUA_ERR_MAX + 1);
  MLI_ASSERT_1(mylua_area, "Couldn't allocate memory. (mylua_area)");

  initid->ptr = (char *)mylua_area;
  return 0;
}

extern "C" void mylua_deinit(UDF_INIT *initid)
{
  mylua_area_dealloc((MYLUA_AREA *)initid->ptr);
  initid->ptr = 0;
}

extern "C" char *mylua(UDF_INIT *initid, UDF_ARGS *args, char *result, unsigned long *length, char *is_null, char *error)
{
  *is_null = 0;
  *length = 0;
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)initid->ptr;
  lua_State *lua = mylua_area->lua;
  char *re_pos = mylua_area->result;

  // 引数
  char *proc = args->args[MYLUA_ARG_PROC];
  char *arg  = args->args[MYLUA_ARG_ARG];

  lua_pushlightuserdata(lua, mylua_area);
  lua_setfield(lua, LUA_REGISTRYINDEX, "mylua_area");

  TABLE_LIST table_list;
  mylua_area->table_list = &table_list;

  // エラー時でもjsonを返す。トップレベルの要素はdata, is_error, messageの3つ
  // mylua.argに引数をセット
  lua_getglobal(lua, "mylua");
  lua_getglobal(lua, "cjson");
  lua_getfield(lua, -1, "decode");
  lua_remove(lua, -2);
  lua_pushstring(lua, arg);
  lua_pcall(lua, 1, 1, 0);
  lua_setfield(lua, -2, "arg");

  lua_getglobal(lua, "cjson");
  lua_getfield(lua, -1, "encode");
  lua_remove(lua, -2);

// goto使うとコンパイルエラーになるのでマクロで。
#define ML_ASSERT(cond, msg) \
  if (cond) { \
  } else { \
    if (mylua_area->index_init_done) { \
      table_list.table->file->ha_index_end(); \
    } \
    if (mylua_area->init_one_table_done) { \
      close_thread_tables(current_thd); \
    } \
    size_t str_c; \
    const char *str = lua_tolstring(lua, -1, &str_c); \
    const char *result = "{\"data\":null,\"is_error\":1,\"message\":\""; \
    *length = strlen(result); \
    *length = *length < MYLUA_ERR_MAX ? *length : MYLUA_ERR_MAX; \
    memcpy(mylua_area->result, result, *length); \
    memcpy(mylua_area->result + *length, msg, MYLUA_ERR_MAX - *length); \
    *length = *length + strlen(msg) < MYLUA_ERR_MAX ? *length + strlen(msg) : MYLUA_ERR_MAX; \
    memcpy(mylua_area->result + *length, str, MYLUA_ERR_MAX - *length); \
    *length = *length + str_c < MYLUA_ERR_MAX ? *length + str_c : MYLUA_ERR_MAX; \
    if (*length + 2 <= MYLUA_ERR_MAX) { \
      mylua_area->result[*length + 0] = '\"'; \
      mylua_area->result[*length + 1] = '}'; \
      mylua_area->result[*length + 2] = '\0'; \
      *length += 2; \
    } else { \
      mylua_area->result[*length - 2] = '\"'; \
      mylua_area->result[*length - 1] = '}'; \
      mylua_area->result[*length - 0] = '\0'; \
    } \
    return mylua_area->result; \
  }

  switch (luaL_loadstring(lua, proc)) {
  case 0: // no error
    break;
  case LUA_ERRSYNTAX:
    ML_ASSERT(0, "luaL_loadstring: LUA_ERRSYNTAX: ");
  case LUA_ERRMEM:
    ML_ASSERT(0, "luaL_loadstring: LUA_ERRMEM: ");
  default:
    ML_ASSERT(0, "luaL_loadstring: default: ");
  }

  switch (lua_pcall(lua, 0, 1, 0)) {
  case 0: // no error
    break;
  case LUA_ERRRUN:
    ML_ASSERT(0, "lua_pcall: LUA_ERRRUN: ");
  case LUA_ERRMEM:
    ML_ASSERT(0, "lua_pcall: LUA_ERRMEM: ");
  case LUA_ERRERR:
    ML_ASSERT(0, "lua_pcall: LUA_ERRERR: ");
  default:
    ML_ASSERT(0, "lua_pcall: defualt: ");
  }
  // TODO: 後処理
  //+int argc = lua_gettop(lua);
  //+char *msg = tcsprintf("Lua error: %s", argc > 0 ? lua_tostring(lua, argc) : "unknown");

  lua_pcall(lua, 1, 1, 0);

  size_t str_c;
  const char *str = lua_tolstring(lua, -1, &str_c);
  if (str_c > mylua_area->result_size) {
    ML_ASSERT(mylua_area_realloc_result(mylua_area, str_c), "mylua_area_realloc_result failed");
  }
  re_pos += sprintf(re_pos, "%s", str);

  if (mylua_area->index_init_done) {
    table_list.table->file->ha_index_end();
  }
  if (mylua_area->init_one_table_done) {
    close_thread_tables(current_thd);
  }

  *length = re_pos - mylua_area->result;
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
  // mysql-5.1.41では、できるだけtable->sを使わないほうがよさそう。
  // table->s->keysとかtable->s->key_infoとかに入ってる値がおかしいので、他のもおかしいかも。
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

  const char *db = lua_tostring(lua, ++argi);
  const char *tbl = lua_tostring(lua, ++argi);
  const char *idx = lua_tostring(lua, ++argi);
  MLIT_ASSERT(db && tbl && idx);
  int fld_0 = argi;
  uint fld_c = argc - argi;

  lua_getfield(lua, LUA_REGISTRYINDEX, "mylua_area");
  MYLUA_AREA *mylua_area = (MYLUA_AREA *)lua_touserdata(lua, -1);

  TABLE_LIST *table_list = mylua_area->table_list;

  // 2回以上init_one_tableして問題ないかわからないので、とりあえずできないように。
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
  memset(mylua_area->keybuf, 0, key->key_length);
  mylua_area->keypart_map = (1 << fld_c) - 1;

  // 面倒なメモリ割り当てを避けるため、ループしなおす
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
  MLIRM_ASSERT(mylua_area->key->key_parts == fld_c);

  TABLE_LIST *table_list = mylua_area->table_list;
  TABLE *table = table_list->table;

  ha_rkey_function ha_read_prefix = (ha_rkey_function)lua_tointeger(lua, ++argi);
  MLIRM_ASSERT(HA_READ_KEY_EXACT <= ha_read_prefix && ha_read_prefix <= HA_READ_MBR_EQUAL);

  size_t offset = 0;
  for (int i = 0; ++argi <= argc; ++i) {
    const char *val = lua_tostring(lua, argi);
    switch (mylua_area->key->key_part[i].type) {
    //case HA_KEYTYPE_TEXT:
    //case HA_KEYTYPE_BINARY:
    //  break;
    case HA_KEYTYPE_SHORT_INT:
    case HA_KEYTYPE_USHORT_INT:
      int2store(mylua_area->keybuf + offset, atoll(val));
      break;
    case HA_KEYTYPE_LONG_INT:
    case HA_KEYTYPE_ULONG_INT:
      int4store(mylua_area->keybuf + offset, atoll(val));
      break;
    case HA_KEYTYPE_LONGLONG:
    case HA_KEYTYPE_ULONGLONG:
      int8store(mylua_area->keybuf + offset, atoll(val));
      break;
    case HA_KEYTYPE_INT24:
    case HA_KEYTYPE_UINT24:
      int3store(mylua_area->keybuf + offset, atoll(val));
      break;
    case HA_KEYTYPE_INT8:
      *((char *)(mylua_area->keybuf + offset)) = atoll(val);
      break;
    default:
      MLIRM_ASSERT(0 && mylua_area->key->key_part[i].type);
      break;
    }
    offset += mylua_area->key->key_part[i].length;
  }

  int read_re = table->file->index_read_map(table->record[0], mylua_area->keybuf, mylua_area->keypart_map, ha_read_prefix);
  lua_pushboolean(lua, read_re == 0);

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

  const char *fld = lua_tostring(lua, ++argi);
  Field *field = mylua_get_field(table, fld);
  MLVI_ASSERT(field);

  lua_pushinteger(lua, field->val_int());

  return 1;
}

//static int mylua_init_index(lua_State *lua) {
//  int argc = lua_gettop(lua);
//  if (argc > 2) {
//  } else {
//    lua_pushstring(lua, "mylua_init_one_table: invalid arguments");
//    lua_error(lua);
//  }
//  table->clear_column_bitmaps();
//}

int luaopen_mylua(lua_State *lua) {
  luaL_Reg reg[] = {
    { "init_table", mylua_init_table },
    { "index_read_map", mylua_index_read_map },
    { "val_int", mylua_val_int },
    { "index_prev", mylua_index_prev },
    { "index_next", mylua_index_next },
    { NULL, NULL }
  };
  luaL_register(lua, "mylua", reg);

  lua_pushliteral(lua, "0.0.1");
  lua_setfield(lua, -2, "version");

  // set mylua.<name> to value of <name>
#define LUAOPEN_MYLUA_SETCONST(name) \
  lua_pushinteger(lua, name); \
  lua_setfield(lua, -2, # name);

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

  return 1;
}

// typedef struct st_key_part_info {	/* Info about a key part */
//   Field *field;
//   uint	offset;				/* offset in record (from 0) */
//   uint	null_offset;			/* Offset to null_bit in record */
//   uint16 length;                        /* Length of keypart value in bytes */
//   /* 
//     Number of bytes required to store the keypart value. This may be
//     different from the "length" field as it also counts
//      - possible NULL-flag byte (see HA_KEY_NULL_LENGTH)
//      - possible HA_KEY_BLOB_LENGTH bytes needed to store actual value length.
//   */
//   uint16 store_length;
//   uint16 key_type;
//   uint16 fieldnr;			/* Fieldnum in UNIREG */
//   uint16 key_part_flag;			/* 0 or HA_REVERSE_SORT */
//   uint8 type;
//   uint8 null_bit;			/* Position to null_bit */
// } KEY_PART_INFO ;

// typedef struct st_key {
//   uint	key_length;			/* Tot length of key */
//   ulong flags;                          /* dupp key and pack flags */
//   uint	key_parts;			/* How many key_parts */
//   uint  extra_length;
//   uint	usable_key_parts;		/* Should normally be = key_parts */
//   uint  block_size;
//   enum  ha_key_alg algorithm;
//   /*
//     Note that parser is used when the table is opened for use, and
//     parser_name is used when the table is being created.
//   */
//   union
//   {
//     plugin_ref parser;                  /* Fulltext [pre]parser */
//     LEX_STRING *parser_name;            /* Fulltext [pre]parser name */
//   };
//   KEY_PART_INFO *key_part;
//   char	*name;				/* Name of key */
//   /*
//     Array of AVG(#records with the same field value) for 1st ... Nth key part.
//     0 means 'not known'.
//     For temporary heap tables this member is NULL.
//   */
//   ulong *rec_per_key;
//   union {
//     int  bdb_return_if_eq;
//   } handler;
//   struct st_table *table;
// } KEY;

// TODO: luaのメモリ使用量を制限できるように

// TODO: 関数を登録（キャッシュ）できるように。全スレーブの関数登録状態を管理するのは難しいので、リクエスト毎に一覧を取得して存在しない関数があったら登録するとか。いまいちかも

// TODO: キャッシュ。コードのMD5をキーにする。キーだけ渡してコードを実行とか。→キャッシュアルゴリズムもluaでかけるように。例えばキャッシュアルゴリズム変更・指定用のAPIを用意しておくとか。やりすぎ？
// TODO: lua_Stateの再利用。初期化後の状態をプールしておく。

// jsonライブラリの比較: http://lua-users.org/wiki/JsonModules
// /usr/lib/liblua5.1.soがないのでシンボリックリンクを張る: http://d.hatena.ne.jp/cou929_la/20080718/1216391301

// cjsonのmake: env LUA_INCLUDE_DIR=../lua/include LUA_LIB_DIR=../lua/lib make
// .aを作る: ライブラリの基礎知識: http://www.hi-ho.ne.jp/babaq/linux/libtips.html
