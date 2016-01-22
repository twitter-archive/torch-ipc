package = "ipc"
version = "scm-1"

source = {
   url = "git://github.com/twitter/torch-ipc.git",
}

description = {
   summary = "A set of primitives for ipc computation in Torch",
   homepage = "-",
   license = "MIT"
}

dependencies = {
   "torch >= 7.0",
   "regress",
}

build = {
   type = "command",
   build_command = [[
cmake -E make_directory build;
cd build;
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(LUA_BINDIR)/.." -DCMAKE_INSTALL_PREFIX="$(PREFIX)" -DCMAKE_C_FLAGS=-fPIC -DCMAKE_CXX_FLAGS=-fPIC;
$(MAKE)
   ]],
   install_command = "cd build && $(MAKE) install"
}
