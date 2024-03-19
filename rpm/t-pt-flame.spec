Summary: Intel PT helper tools for performance analysis
Name: t-pt-flame
Version: 1.0.0
Release: %(echo $RELEASE)%{?dist}
License: Commercial
Group: alibaba/application
URL: https://code.alibaba-inc.com/sunjingyuan.sjy/pt_flame

BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root
Prefix: /usr/share/pt_flame

BuildRequires: devtoolset-7-gcc
BuildRequires: devtoolset-7-gcc-c++
BuildRequires: devtoolset-7-binutils
BuildRequires: devtoolset-7-libatomic-devel
BuildRequires: kernel-headers = 5.10.112

Requires: jemalloc
Requires: perl
Requires: perf

%description
pt_flame provides flamegraph, perfetto and print stack utilities for Intel PT

%prep

%build

cd $OLDPWD/../

export CC=/opt/rh/devtoolset-7/root/usr/bin/gcc
export CXX=/opt/rh/devtoolset-7/root/usr/bin/g++
cmake . -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=%{prefix}

cmake --build .

%install

cd $OLDPWD/../
make DESTDIR=$RPM_BUILD_ROOT -j install

%clean
rm -rf $RPM_BUILD_ROOT

%files
%{prefix}

%changelog
