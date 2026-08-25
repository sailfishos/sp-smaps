Name:       sp-smaps
Summary:    Utilities for collecting whole system SMAPS data
Version:    0.4.2.2
Release:    1
License:    GPLv2
URL:        https://github.com/sailfishos/sp-smaps
Source0:    %{name}-%{version}.tar.gz
BuildRequires:  libsysperf-devel

%description
Utilities for collecting whole system SMAPS data and post-processing the information in it to cross-linked HTML tables

%package doc
Summary:   Documentation for %{name}
BuildArch: noarch
Requires:  %{name} = %{version}-%{release}

%description doc
Man pages for %{name}.

%package tests
Summary:   Tests for %{name}
BuildArch: noarch
Requires:  %{name} = %{version}-%{release}
Requires: blts-tools

%description tests
Tests for %{name}.

%prep
%autosetup -n %{name}-%{version}

%build
# building is done in during install. Empty build section to avoid rpmlint warning

%install
%make_install

%files
%defattr(-,root,root,-)
%license COPYING
%{_bindir}/sp_smaps_analyze
%{_bindir}/sp_smaps_appvals
%{_bindir}/sp_smaps_diff
%{_bindir}/sp_smaps_expand
%{_bindir}/sp_smaps_filter
%{_bindir}/sp_smaps_flatten
%{_bindir}/sp_smaps_normalize
%{_bindir}/sp_smaps_snapshot
%{_bindir}/sp_smaps_sorted_totals
%dir %{_datadir}/sp-smaps-visualize
%{_datadir}/sp-smaps-visualize/asc.gif
%{_datadir}/sp-smaps-visualize/bg.gif
%{_datadir}/sp-smaps-visualize/desc.gif
%{_datadir}/sp-smaps-visualize/expander.js
%{_datadir}/sp-smaps-visualize/jquery.metadata.js
%{_datadir}/sp-smaps-visualize/jquery.min.js
%{_datadir}/sp-smaps-visualize/jquery.tablesorter.js
%{_datadir}/sp-smaps-visualize/tablesorter.css

%files doc
%defattr(-,root,root,-)
%doc README.txt
%{_mandir}/man1/sp_smaps*

%files tests
%defattr(-,root,root,-)
/opt/tests/%{name}/*
