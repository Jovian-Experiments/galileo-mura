project('galileo-mura-extractor', ['c'], default_options: ['warning_level=3'])

executable('galileo-mura-extractor', 'main.c', install: true, override_options: [ 'c_std=c11' ] )

install_data('galileo-mura-setup', install_dir : get_option('bindir'))
install_data('galileo-mura-download', install_dir : get_option('bindir'))
