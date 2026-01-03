project('pipetron', 'cpp')

pipewire_dep = dependency('libpipewire-0.3')

executable('pipetron', 'src/main.cpp', dependencies: [pipewire_dep], install: true)

systemd_dep = dependency('systemd')
systemd_user_dir = systemd_dep.get_variable('systemduserunitdir')

conf_data = configuration_data()
conf_data.set('BINARY_PATH', get_option('prefix') / get_option('bindir') / 'pipetron')

configure_file(
    output: 'pipetron.service',
    input: 'src/systemd/user/pipetron.service.in',
    configuration: conf_data,
    install_dir: systemd_user_dir,
)