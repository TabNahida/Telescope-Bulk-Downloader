add_languages("cxx20")

target("TESS-Downloader")
    set_kind("binary")
    set_encodings("utf-8")

    add_includedirs("include")
    add_files("src/tess.cpp")

target_end()