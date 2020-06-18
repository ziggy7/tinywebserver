server: main.cpp ./threadpool/threadpool.h ./http_conn/http_conn.cpp ./http_conn/http_conn.h ./lock/locker.h  ./CGImysql/sql_connection_pool.cpp ./CGImysql/sql_connection_pool.h
	g++ -o server -g main.cpp ./lock/locker.h ./http_conn/http_conn.cpp ./http_conn/http_conn.h  ./CGImysql/sql_connection_pool.cpp ./CGImysql/sql_connection_pool.h ./threadpool/threadpool.h  -lpthread -lmysqlclient



clean:
	rm  -r server

