--TEST--
xdebug.mode=auto resolves heuristically; XDEBUG_MODE and explicit modes win
--SKIPIF--
<?php if (!extension_loaded('xdebug') || !function_exists('fast_xdebug_resolved_mode')) echo 'skip fast-xdebug not loaded'; ?>
--INI--
xdebug.mode=auto
--ENV--
XDEBUG_MODE=profile
--FILE--
<?php
// With xdebug.mode=auto and XDEBUG_MODE=profile in the environment, the
// resolved mode must be "profile".
var_dump(fast_xdebug_resolved_mode());
var_dump(in_array('profile', xdebug_info('mode'), true));
?>
--EXPECT--
string(7) "profile"
bool(true)
