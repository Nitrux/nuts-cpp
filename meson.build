project('nuts', 'cpp', version: '3.0.0', meson_version: '>=1.12.0', default_options: ['cpp_std=c++17', 'buildtype=release'])

qt6 = import('qt6')
qt = dependency('qt6', version: '>=6.9', modules: ['Core', 'Concurrent', 'DBus', 'Network', 'Quick', 'Qml'])
kf6_core = dependency('KF6CoreAddons', version: '>=6.13')
cpp_compiler = meson.get_compiler('cpp')
if cpp_compiler.has_argument('-Wno-sfinae-incomplete')
    add_project_arguments('-Wno-sfinae-incomplete', language: 'cpp')
endif
kf6_i18n = declare_dependency(
    include_directories: include_directories(get_option('prefix') / 'include' / 'KF6' / 'KI18n'),
    dependencies: [cpp_compiler.find_library('KF6I18n'), cpp_compiler.find_library('KF6I18nQml')],
)
kf6_notifications = declare_dependency(
    include_directories: include_directories(get_option('prefix') / 'include' / 'KF6' / 'KNotifications'),
    dependencies: [cpp_compiler.find_library('KF6Notifications')],
)
polkit = declare_dependency(
    include_directories: include_directories(get_option('prefix') / 'include' / 'polkit-qt6-1'),
    dependencies: [cpp_compiler.find_library('polkit-qt6-core-1')],
)
mauikit = dependency('MauiKit4', version: '>=4.0.5')

git_program = find_program('git', required: false)
git_branch = 'Stable'
git_commit_hash = '3.0.0'
if git_program.found()
    git_branch_result = run_command(git_program, '-C', meson.project_source_root(), 'rev-parse', '--abbrev-ref', 'HEAD', check: false)
    git_commit_result = run_command(git_program, '-C', meson.project_source_root(), 'rev-parse', '--short', 'HEAD', check: false)
    if git_branch_result.returncode() == 0 and git_branch_result.stdout().strip() != ''
        git_branch = git_branch_result.stdout().strip()
    endif
    if git_commit_result.returncode() == 0 and git_commit_result.stdout().strip() != ''
        git_commit_hash = git_commit_result.stdout().strip()
    endif
endif

cpp_args = ['-DGIT_BRANCH="' + git_branch + '"', '-DGIT_COMMIT_HASH="' + git_commit_hash + '"']
if host_machine.cpu_family() == 'x86_64'
    cpp_args += ['-march=x86-64-v3']
endif

nuts_headers = files(
    'include/nuts/NutsExport.h',
    'include/nuts/Types.h',
    'include/nuts/Config.h',
    'include/nuts/Logger.h',
    'include/nuts/SystemInterface.h',
    'include/nuts/BackupManager.h',
    'include/nuts/UpdateManager.h',
)
nuts_moc = qt6.preprocess(moc_headers: ['include/nuts/Config.h', 'include/nuts/Logger.h', 'include/nuts/SystemInterface.h', 'include/nuts/BackupManager.h', 'include/nuts/UpdateManager.h'])
nuts = shared_library(
    'nuts',
    files('src/lib/Config.cpp', 'src/lib/Logger.cpp', 'src/lib/SystemInterface.cpp', 'src/lib/BackupManager.cpp', 'src/lib/UpdateManager.cpp') + nuts_headers + nuts_moc,
    dependencies: [qt, kf6_core],
    include_directories: include_directories('include'),
    cpp_args: cpp_args + ['-DNUTS_LIBRARY'],
    version: '3.0.0',
    soversion: '3',
    install: true,
)
install_subdir('include/nuts', install_dir: get_option('includedir'))

agent = executable('nuts-agent', files('src/agent/main.cpp', 'src/agent/NutsAgent.cpp'), dependencies: [qt], include_directories: include_directories('include'), cpp_args: cpp_args, install: true, install_dir: get_option('sbindir'))

qdbusxml2cpp = find_program('qdbusxml2cpp', dirs: ['/usr/lib/qt6/bin', '/usr/lib/qt6/libexec'])
helper_adaptor = custom_target('nuts-helper-dbus', input: 'data/dbus/org.nxos.nuts.xml', output: ['nutsadaptor.cpp', 'nutsadaptor.h'], command: [qdbusxml2cpp, '-a', 'nutsadaptor', '@INPUT@'])
helper_moc = qt6.preprocess(moc_headers: ['src/helper/NutsHelper.h', helper_adaptor[1]])
helper = executable('nuts-helper', files('src/helper/main.cpp', 'src/helper/NutsHelper.cpp') + helper_adaptor + helper_moc, dependencies: [qt, polkit, kf6_core], link_with: nuts, include_directories: include_directories('include'), cpp_args: cpp_args, install: true, install_dir: get_option('libexecdir'))

gui_moc = qt6.preprocess(qresources: 'src/gui/qml.qrc', moc_headers: ['src/gui/NutsClient.h'])
gui = executable('nuts-gui', files('src/gui/main.cpp', 'src/gui/NutsClient.cpp') + gui_moc, dependencies: [qt, kf6_core, kf6_i18n, kf6_notifications, mauikit], link_with: nuts, include_directories: include_directories('include'), cpp_args: cpp_args, install: true)
install_data('src/gui/org.nxos.nuts.desktop', install_dir: get_option('datadir') / 'applications')

service = configure_file(input: 'data/dbus/org.nxos.nuts.service.in', output: 'org.nxos.nuts.service', configuration: {'KDE_INSTALL_FULL_LIBEXECDIR': get_option('prefix') / get_option('libexecdir')})
install_data(service, install_dir: get_option('datadir') / 'dbus-1' / 'system-services')
install_data('data/dbus/org.nxos.nuts.conf', install_dir: get_option('sysconfdir') / 'dbus-1' / 'system.d')
install_data('data/dbus/org.nxos.nuts.xml', install_dir: get_option('datadir') / 'dbus-1' / 'interfaces')
install_data('data/polkit/org.nxos.nuts.policy', install_dir: get_option('datadir') / 'polkit-1' / 'actions')
