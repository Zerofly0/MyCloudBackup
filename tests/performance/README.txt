使用方法

1. 将 performance_test.ps1 和 run_performance_test.bat 放在同一个文件夹。
2. 双击 run_performance_test.bat。
3. 默认测试参数：
   - 服务器：http://8.137.100.109:8080
   - 测试文件：100 MB
   - 并发数：10
   - 每个并发执行 5 次请求
4. 测试完成后，会生成 performance_results_时间戳 文件夹。
5. 结果文件：
   - summary.csv：汇总结果
   - transfer_metrics.csv：上传/下载指标
   - health_requests.csv：健康检查并发明细
   - list_requests.csv：列表查询并发明细
   - upload_response.json：上传接口响应

修改参数的方法

在 CMD 中运行：
powershell.exe -NoProfile -ExecutionPolicy Bypass -File performance_test.ps1 -FileSizeMB 50 -Concurrency 5 -RequestsPerWorker 10

保留本地测试大文件：
powershell.exe -NoProfile -ExecutionPolicy Bypass -File performance_test.ps1 -KeepFiles

更换服务器：
powershell.exe -NoProfile -ExecutionPolicy Bypass -File performance_test.ps1 -BaseUrl "http://你的IP:8080"

注意
- 默认只上传一个 100 MB 文件，随后会删除云端测试文件。
- 并发部分只测试 /health 和 /list，不会批量上传大量文件。
- 请先确认服务器在线，并避免在多人正在使用服务器时设置过高并发。
