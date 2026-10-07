Import("env")
env.Append(CPPPATH=["$PROJECT_DIR/core/include"])
env.BuildSources("$BUILD_DIR/nodx-core", "$PROJECT_DIR/core/src")
