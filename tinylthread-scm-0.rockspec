package = "tinylthread"
version = "scm-0"
source = {
  url = "gitrec+https://github.com/siffiejoe/lua-tinylthread"
}
description = {
  summary = "Tiny and portable multi-threading module for Lua.",
  homepage = "https://github.com/siffiejoe/lua-tinylthread/",
  license = "MIT+ZLIB"
}

supported_platforms = { "linux", "windows", "macosx" }
dependencies = {
  "lua >= 5.1, < 5.4",
  "luarocks-fetch-gitrec",
}

build = {
  type = "builtin",
  modules = {
    tinylthread = {
      sources = { "tinylthread.c" },
      defines = {
        --"NDEBUG"
      },
      incdirs = { "tinycthread/source" },
    },
  },
  platforms = {
    linux = {
      modules = {
        tinylthread = {
          libraries = { "pthread", "rt" },
        },
      },
    },
  },
}

