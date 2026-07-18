@echo off
rem Dews Feed work-task sync: pushes WORK_TASKS.md to the office screen.
rem Runs every 15 min via Task Scheduler. Works from anywhere (Tailscale IP).
C:\Windows\System32\OpenSSH\scp.exe -i C:\Users\dewit\.ssh\dewsfeed_pi -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new "C:\Users\dewit\OneDrive\Desktop\Claude-Workspace\projects\Patriot Bank\WORK_TASKS.md" dew@100.65.128.36:/home/dew/dews_feed_web/work_tasks.md
