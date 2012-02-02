<?php

error_reporting(E_ALL);
set_error_handler('error_handler');
set_exception_handler('exception_handler');

$command = isset($argv[1]) ? $argv[1] : '';

$all_command_m = array(
    "prepare" => array(),
    "prepare_lua" => array(),
    "prepare_luajit" => array(),
    "prepare_lua_cjson" => array(),
    "prepare_mysql" => array(),
    "prepare_mylua" => array(),
    "install" => array("soname", "udfname", "plugin_dir", "mysql_host", "user"),
    "uninstall" => array("soname", "udfname", "plugin_dir", "mysql_host", "user"),
    "replace" => array("soname", "udfname", "plugin_dir", "mysql_host", "user"),
);

$root_command_m = map(array(
    "install",
    "uninstall",
    "replace",
));

if (isset($all_command_m[$command])) {
} else {
    print_usage($all_command_m);
    exit(1);
}

if (isset($root_command_m[$command])) {
    if (trim(`id -u`) === "0") {
    } else {
        print "please run on root user.\n";
        exit(1);
    }
}

$arg_a = $all_command_m[$command];
$a = array();
for ($i = 0; $i < count($arg_a); ++$i) {
    if (!isset($argv[$i + 2])) {
        print "not enough arguments.\n";
        print_usage($all_command_m);
        exit(1);
    }
    $a[$arg_a[$i]] = $argv[$i + 2];
}

$command($a);
print "\ndone. (ok)\n";
exit();

// usage
function print_usage($all_command_m) {
    $base = basename(__FILE__);
    print "usage:\n";
    foreach ($all_command_m as $cmd => $args) {
        print "  php ".$base." ".$cmd;
        foreach ($args as $arg) {
            print " <".$arg.">";
        }
        print "\n";
    }
}

// command
function prepare() {
    prepare_lua();
    prepare_luajit();
    prepare_lua_cjson();
    prepare_mysql();
    prepare_mylua();
}

function prepare_lua() {
    my_exec("wget http://www.lua.org/ftp/lua-5.1.4.tar.gz");
    my_exec("tar zxf lua-5.1.4.tar.gz");
    my_exec("ln -s lua-5.1.4 lua");
    my_cd("lua");
    my_exec("sed -i 's/MYCFLAGS=-DLUA_USE_POSIX/MYCFLAGS=\"-DLUA_USE_POSIX -fPIC\"/g' src/Makefile");
    my_exec("make posix");
    my_exec("make local");
    my_cd("..");
}

function prepare_luajit() {
    my_exec("wget http://luajit.org/download/LuaJIT-2.0.0-beta9.tar.gz");
    my_exec("tar zxf LuaJIT-2.0.0-beta9.tar.gz");
    my_exec("ln -s LuaJIT-2.0.0-beta9 luajit");
    my_cd("luajit");
    my_exec("sed -i 's/STATIC_CC = $(CROSS)$(CC)/STATIC_CC = $(CROSS)$(CC)/g' src/Makefile");
    my_exec("make");
    my_exec("make install PREFIX=`pwd`");
    my_cd("..");
}

function prepare_lua_cjson() {
    my_exec("wget http://www.kyne.com.au/~mark/software/download/lua-cjson-1.0.4.tar.gz");
    my_exec("tar zxf lua-cjson-1.0.4.tar.gz");
    my_exec("ln -s lua-cjson-1.0.4 lua-cjson");
    my_cd("lua-cjson");
    my_exec("env LUA_INCLUDE_DIR=../lua/include LUA_LIB_DIR=../lua/lib make");
    my_exec("ar rv cjson.a lua_cjson.o strbuf.o");
    my_cd("..");
}

function prepare_mysql() {
    $output = my_exec("mysql --version");
    preg_match('/Distrib ([0-9.]+),/u', $output, $version);
    $version = $version[1];
    my_exec("wget http://downloads.mysql.com/archives/mysql-5.1/mysql-{$version}.tar.gz");
    my_exec("tar zxf mysql-{$version}.tar.gz");
    my_exec("ln -s mysql-{$version} mysql");
    my_cd("mysql");
    my_exec("./configure");
    my_exec("cp include/config.h include/my_config.h");
    my_cd("..");
}

function prepare_mylua($a) {
    my_exec(implode(" ", array(
        "g++ -O2 -lm -ldl -Wall -nostartfiles -shared -fPIC",
        "-L /usr/lib",
        "-I ./mysql/include -I ./mysql/sql -I ./mysql/regex",
        "-I ./lua/include",
        "src/mylua.cc lua/lib/liblua.a lua-cjson/cjson.a",
        "-o mylua.so",
    )));
}

function prepare_mylua_with_luajit($a) {
    my_exec(implode(" ", array(
        "g++ -O2 -lm -ldl -Wall -nostartfiles -shared -fPIC",
        "-L /usr/lib",
        "-I ./mysql/include -I ./mysql/sql -I ./mysql/regex",
        "-I ./luajit/include/luajit-2.0",
        "src/mylua.cc luajit/lib/libluajit-5.1.a lua-cjson/cjson.a",
        "-D MYLUA_USE_LUAJIT",
        "-o mylua.so",
    )));
}

function install($a) {
    $a["pass"] = read_pass("mysql password (".$a["user"]."): ");
    mysql_connect($a["mysql_host"], $a["user"], $a["pass"]);
    mysql_set_charset("utf8");
    install_common($a["soname"], $a["udfname"], $a["plugin_dir"], $a["mysql_host"], $a["user"], $a["pass"]);
    test_common($a["soname"], $a["udfname"], $a["plugin_dir"], $a["mysql_host"], $a["user"], $a["pass"]);
}

function uninstall($a) {
    $a["pass"] = read_pass("mysql password (".$a["user"]."): ");
    mysql_connect($a["mysql_host"], $a["user"], $a["pass"]);
    mysql_set_charset("utf8");
    uninstall_common($a["soname"], $a["udfname"], $a["plugin_dir"], $a["mysql_host"], $a["user"], $a["pass"]);
}

function replace($a) {
    $a["pass"] = read_pass("mysql password (".$a["user"]."): ");
    mysql_connect($a["mysql_host"], $a["user"], $a["pass"]);
    mysql_set_charset("utf8");
    uninstall_common($a["soname"], $a["udfname"], $a["plugin_dir"], $a["mysql_host"], $a["user"], $a["pass"]);
    install_common($a["soname"], $a["udfname"], $a["plugin_dir"], $a["mysql_host"], $a["user"], $a["pass"]);
    test_common($a["soname"], $a["udfname"], $a["plugin_dir"], $a["mysql_host"], $a["user"], $a["pass"]);
}

// common
function install_common($soname, $udfname, $plugin_dir, $host, $user, $pass) {
    my_exec("sudo cp ".qsh($soname)." ".qsh($plugin_dir));
    my_query("create function $udfname returns string soname ".q($soname));
}

function uninstall_common($soname, $udfname, $plugin_dir, $host, $user, $pass) {
    my_exec("sudo cp ".qsh($plugin_dir."/".$soname)." ".qsh($soname.".bak"));
    my_query("drop function if exists $udfname");
}

function test_common($soname, $udfname, $plugin_dir, $host, $user, $pass) {
    $re = my_query("select $udfname('return mylua.arg.test', ".q(json_encode(array("test" => 3))).") as json");
    $row = mysql_fetch_assoc($re);
    if (isset($row["json"])); else error_exit("test failed.");
    $result = json_decode($row["json"], 1);
    if ($result["data"] === 3); else error_exit("test failed. data=".var_export($data, 1));
}

// util
function qsh($s) {
    return escapeshellarg($s);
}

function q($s) {
    return "'".mysql_real_escape_string($s)."'";
}

function my_query($sql) {
    $re = mysql_query($sql);
    if ($re); else error_exit($sql."\n(".mysql_errno().") ".mysql_error());
    return $re;
}

function my_cd($dir) {
    print "cd ".qsh($dir)."\n";
    chdir($dir);
}

function my_exec($cmd) {
    print "exec: $cmd 2>&1\n";
    exec("$cmd 2>&1", $stdout, $ret);
    $stdout = implode("\n", $stdout)."\n";
    print $stdout;
    if ($ret) error_exit("command failed: status=$ret\n");
    return $stdout;
}

function error_exit($s) {
    $trace = debug_backtrace();
    print "error: L".$trace[0]["line"].": ".$s."\n";
    exit(1);
}

function error_exit_mysql($s) {
    print "error: ".$s.": (".mysql_errno().") ".mysql_error()."\n";
    exit(1);
}

function read_input($str, $default) {
    print $str;
    $input = trim(fgets(STDIN));
    return $input === "" ? $default : $input;
}

function stty_echo() {
    `stty echo`;
}

function read_pass($str) {
    print $str;
    register_shutdown_function('stty_echo');
    `stty -echo`;
    $pass = trim(fgets(STDIN));
    `stty echo`;
    print "\n";
    return $pass;
}

function map($a) {
    $m = array();
    foreach ($a as $x) $m[$x] = $x;
    return $m;
}

function error_handler($n, $s, $f, $l) {
    throw new ErrorException($s, 0, $n, $f, $l);
}

function exception_handler($e) {
    print $e->getMessage();
    print $e->getTraceAsString();
}
