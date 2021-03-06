set(LLVM_NO_RTTI 1)

set( LLDB_USED_LIBS
  lldbAPI
  lldbBreakpoint
  lldbCommands
  lldbCore
  lldbExpression
  lldbHostCommon
  lldbInitAndLog
  lldbInterpreter
  lldbSymbol
  lldbTarget  
  lldbUtility
  
  # Plugins
  lldbPluginDisassemblerLLVM
  lldbPluginSymbolFileDWARF
  lldbPluginSymbolFileSymtab
  
  #lldbPluginObjectFileELF
  #lldbPluginObjectContainerBSDArchive
  #lldbPluginSymbolVendorMacOSX
  
  # Windows
  lldbPluginProcessGDBRemote
  lldbHostWindows
  lldbPluginPlatformWindows
  lldbPluginObjectFilePECOFF
  lldbPluginObjectFileMachO
  lldbPluginPlatformGDB
  lldbPluginProcessUtility
  lldbPluginDynamicLoaderStatic
  
  # Linux
  #lldbHostLinux
  
  Ws2_32
  )
  
set( CLANG_USED_LIBS
  clangAnalysis
  clangAST
  clangBasic
  clangCodeGen
  clangDriver
  clangEdit
  clangFrontend
  clangLex
  clangParse
  clangRewriteCore
  clangRewriteFrontend
  clangSema
  clangSerialization
  )  
  
set( LLDB_DRIVER_LIBS
  #edit
  #python2.6
  )

set( LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  jit
  interpreter
  nativecodegen
  asmparser
  bitreader
  bitwriter
  codegen
  ipo
  selectiondag
  bitreader
  mc
  core
  )

add_lldb_executable(lldb
  Driver.cpp
  DriverEvents.cpp
  DriverOptions.cpp
  DriverPosix.cpp
  IOChannel.cpp
  )

install(TARGETS lldb
  RUNTIME DESTINATION bin)
