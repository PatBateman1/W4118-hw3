This file should contain:

  - Your name & UNI (or those of all group members for group assignments)
  - Homework assignment number
  - Description for each part

The description should indicate whether your solution for the part is working
or not. You may also want to include anything else you would like to
communicate to the grader, such as extra functionality you implemented or how
you tried to fix your non-working code.

part1:
  my solution works.
  I create child process after accept, close server socket in child process and 
  close client socket in parent process. After send, close client process in 
  parent process.

part2:
  my solution works.
  I use mmap to create a shared memory for processes to access the stats,.
  I put sem_tstructure in the struct data and use malloc to allocate memory for it. 
  I use a binary semaphore. 
  The sem_wait() is a slow system call.

part3:
  my solution works.
  I call fork in the if condition (when it comes to directory listing), I use
  child process to call execl and parent process to receive the output. I make 
  a pipe and in child process, close the read end and overwrite the standard output
  and standard error with the pipe write end, in parent process, close the write 
  end so that I can read the output sent from child process.

part5:
  my solution works
  compared to part0 and part1, part5 improved a little bit.
  I use pthread_detach() since if I use pthread_join(), it would block the main thread,
  so the user can not connect to the server.

  the two non-thread-safe function is:
  strtok, I replace it with strtok_r.
  inet_ntoa, I replace it with inet_ntop.

part6:
  my solution works

part7:
  my solution works
  I choose to call pthread_cond_broadcast since there are many threads wait 
  for the queue. 
  to get clntAddr, I use getpeername in the thread function.
  the performance of part7 is better compared to part6

part8:
  my solution works
  compared to part7, the performance of part8 is worse
  when memset set to -1, it will fill the byte 1 byte by byte, so the int is set
  to -1.
  when the select() returns correctly, I traverse the servSocks to see if there 
  is a connect request.

part10:
  my solution works
  I make struct data pointer a global variable so the signal handler can reach it.

part12:
  my solution works
  I use waitpid(-1, NULL, 0) to wait for the child process.
  I use sigprocmast to block the signal before fork and unblock in parent process 
  after fork.

part13:
  my solution works
  In the for loop to create process pool, I also create the socket connection, I 
  close the read end in parent process and close write end in child process.




