#pragma once
// 复制本文件为 secrets.h 后填写（secrets.h 已被 .gitignore 排除，不会进仓库）
//
// WIFI_SSID 留空时会尝试沿用设备 NVS 里保存的凭据（来自 wb-status 固件），
// 都拿不到才进离线演示模式。
#define WIFI_SSID ""
#define WIFI_PASS ""
// Mac 桥接服务地址，启动 bridge.py 时会打印局域网 IP
// ⚠️ 这是 DHCP 地址，Mac 换网络或续租后会变；建议在路由器上给它绑定静态 IP
#define BRIDGE_BASE "http://192.168.1.100:8787"
