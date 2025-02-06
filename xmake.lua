add_languages("cxx20")
add_requires("cpp-httplib", {configs = {ssl = true}})

target("TESS_Downloader")
    set_kind("binary")
    set_encodings("utf-8")

    add_packages("cpp-httplib")
    --add_includedirs("include")
    add_files("src/tess.cpp")

target_end()