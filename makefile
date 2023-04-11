build:
	@echo "开始编译"
	g++ -o result main.cpp threadpool.cpp -L/usr/lib/x86_64-linux-gnu -lpthread
	@echo "编译完成"