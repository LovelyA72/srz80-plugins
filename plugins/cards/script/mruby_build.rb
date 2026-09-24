if ENV['SRZ80_MRUBY_CROSS'] == '1'
  MRuby::Build.new('host') do |conf|
    conf.toolchain :gcc
    conf.gem :core => 'mruby-compiler'
  end
  MRuby::CrossBuild.new('target') do |conf|
    conf.toolchain :gcc
    conf.host_target = 'x86_64-w64-mingw32'
    conf.cc.command = ENV.fetch('SRZ80_MRUBY_CC')
    conf.linker.command = conf.cc.command
    conf.archiver.command = ENV.fetch('SRZ80_MRUBY_AR')
    conf.exts.executable = '.exe'
    conf.cc.defines << 'MRB_ENABLE_DEBUG_HOOK'
    conf.gem :core => 'mruby-compiler'
  end
else
  MRuby::Build.new do |conf|
    conf.toolchain
    conf.cc.command = ENV.fetch('SRZ80_MRUBY_CC')
    conf.linker.command = conf.cc.command
    conf.archiver.command = ENV.fetch('SRZ80_MRUBY_AR')
    conf.cc.flags << '-fPIC'
    conf.cc.defines << 'MRB_ENABLE_DEBUG_HOOK'
    conf.gem :core => 'mruby-compiler'
  end
end
