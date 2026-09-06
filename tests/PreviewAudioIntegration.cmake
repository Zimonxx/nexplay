# These are synthetic fixtures in the build directory, never user recordings.
file(MAKE_DIRECTORY "${TEST_DIR}")
foreach(variant IN ITEMS ordinary reordered)
    set(fixture "${TEST_DIR}/${variant}.mp4")
    if(variant STREQUAL "ordinary")
        set(mapping -map 0:v -map 1:a -map 2:a)
        set(identities 2 440 3 880)
        set(options)
    else()
        # Audio before video, nonconsecutive IDs, duplicate labels and a different
        # default track. Neither ordering nor labels can identify the right sound.
        set(mapping -map 2:a -map 0:v -map 1:a)
        set(identities 42 440 13 880)
        set(options -use_stream_ids_as_track_ids 1
            -streamid 0:13 -streamid 1:7 -streamid 2:42
            -disposition:a:0 0 -disposition:a:1 default -movflags +faststart)
    endif()
    execute_process(COMMAND "${FFMPEG}" -hide_banner -loglevel error -y
        -f lavfi -i "testsrc2=size=160x90:rate=30:duration=3"
        -f lavfi -i "sine=frequency=440:duration=3"
        -f lavfi -i "sine=frequency=880:duration=3"
        ${mapping} -c:v mpeg4 -c:a aac
        -metadata:s:a:0 "handler_name=Same name"
        -metadata:s:a:1 "handler_name=Same name"
        ${options} "${fixture}" RESULT_VARIABLE generated)
    if(NOT generated EQUAL 0)
        message(FATAL_ERROR "Cannot generate ${variant} audio identity fixture")
    endif()
    file(SHA256 "${fixture}" before)
    execute_process(COMMAND "${TEST_EXE}" "${fixture}" ${identities} --decode-only
        RESULT_VARIABLE tested)
    if(NOT tested EQUAL 0)
        message(FATAL_ERROR "Audio identity regression: ${variant}")
    endif()
    file(SHA256 "${fixture}" after)
    if(NOT before STREQUAL after)
        message(FATAL_ERROR "Audio preview modified the original recording")
    endif()
endforeach()
