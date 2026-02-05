project('pipetron', 'cpp', version: '1.1.0', license: 'MIT')

project_name = meson.project_name()
project_version = meson.project_version()
project_license = meson.project_license()[0]

pipewire_dep = dependency('libpipewire-0.3')

project_conf_data = configuration_data()
project_conf_data.set_quoted('PROJECT_NAME', project_name)
project_conf_data.set_quoted('PROJECT_VERSION', project_version)
project_conf_data.set_quoted('PROJECT_LICENSE', project_license)

configure_file(
    output: 'config.h',
    configuration: project_conf_data,
)

project_include_dir = include_directories('include')

executable(
    'pipetron',
    'src/main.cpp',
    include_directories: project_include_dir,
    dependencies: [pipewire_dep],
    install: true,
)

systemd_dep = dependency('systemd')
systemd_user_dir = systemd_dep.get_variable('systemduserunitdir')

systemd_conf_data = configuration_data()
systemd_conf_data.set('BINARY_PATH', get_option('prefix') / get_option('bindir') / 'pipetron')

configure_file(
    output: 'pipetron.service',
    input: 'src/systemd/user/pipetron.service.in',
    configuration: systemd_conf_data,
    install_dir: systemd_user_dir,
)