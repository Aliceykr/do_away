@echo off
set "NPM_CONFIG_CACHE=C:\Users\Aliceykr\Desktop\do\do_away\MDK-ARM\.npm-cache"
if not exist "%NPM_CONFIG_CACHE%" mkdir "%NPM_CONFIG_CACHE%"
npx -y mcp-remote https://rube.app/mcp
