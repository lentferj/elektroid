TESTS = sample_fs_tests.sh data_fs_tests.sh

EXTRA_DIST = \
        $(TESTS) \
        res/square.wav

AM_TESTS_ENVIRONMENT = \
	ecli='$(abs_top_builddir)'/src/elektroid-cli; \
	export ecli;
