# DnsFilter
This is a simple dns filter. It works as transparent proxy. You must change default DNS ip&ip6 address at network adaptor settings by 127.0.0.1 and ::1 for ip4 and ip6 respectively and start up this program.

Program has one parameter: it is IP4 address of real dns server. If you don't point it then the 8.8.8.8 will be used.

At the same directory where dnsfilter.exe is the config.ini must be located. Config.ini contains one line per allowed domain mask. Mask determines allowed domain and subdomains.
