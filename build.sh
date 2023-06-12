#编译成可执行程序
#g++ main.cpp threadpool.cpp -lpthread -std=c++17
#编译成动态链接库
g++ -fPIC -shared threadpool.cpp -o libmypool.so -std=c++17 -lpthread
#使用动态链接库
#g++ main.cpp -std=c++17 -lpthread -L./ -lmypool -o a.out

#./a.out 报错：修改步骤：
#1.cd /etc/ld.so.conf.d
#2.touch mylib.conf
#3.sudo vim mylib.conf; 写上自己的动态库的路径 /home/sjj/my_study_project/xgg-threadpool
#4.执行下ldconfig  相当于把新加的配置信息刷到/etc/ld.so.cache中  视频29
#完成上述步骤 ./a.out 没有问题了；