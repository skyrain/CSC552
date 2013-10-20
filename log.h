#ifndef LOG_H
#define LOG_H


#include "flash.h"
#include <time.h>
#include <sys/types.h>

#define DIRECT_BK_NUM 4
#define FILE_NAME_LENGTH 8
//#define BLOCK_SIZE FLASH_BLOCK_SIZE // temp for file layer test

//#define N_BEGIN_BLOCK 2

//----------------gloabal value-----------------------
Super_log_seg * super_log_seg;
Disk_cache * disk_cache;

//-------points to the logAddress that could start to write data-------
LogAddress tail_log_addr;

u_int wearlimit;

//----flash memory name---------------
char * fl_file;          

//--------default: 1024-------------
//-------- chosen by user-------------
u_int sec_num;

//-------default: 2-----------------
//-------2 sectors = 1 block----------
u_int bk_size;

//------default: 32--------------------------
//---------chose by user-------------------
u_int bks_per_seg;

u_int seg_size;

//??注意应写程序保证若用户输入导致计算出的seg_num非整数则让用户重新输
//入
//-----------total seg num-------------
u_int seg_num;

//------  default: 2-----------------------
//------ chose by user--------------------
u_int flie_bk_size;


//--------default :4 ------------------
//------chosen by user----------------
u_int cache_seg_num;   

//-------------------------------------------------------------



//------------------structure definition---------------------

//----------------------------------------------------
typedef struct Block
{
    u_int bk_no;

/*    
    //--------the bk is pointed by which file's which bk
    //--------initialize to be -1, means no file points to 
    //------this bk now;
    u_int ino;
    u_int ino_bk_no;
*/

    //-----存file data------
    //----可以存的大小为
    // 一个block大小 - (Block令两个属性的bytes,sizeof 计算)
    void * bk_content;   
   
    struct Block * next;

}Block;

//list "bk_no"th block in certain seg
//it contaiins which file's which file block data
typedef struct Seg_sum_entry
{
    u_int bk_no;
    u_int file_no;            
    u_int file_bk_no;

    struct Seg_sum_entry * next;

}Seg_sum_entry;

//每个seg有 bks_per_seg 个 blocks
typedef struct Seg_sum_bk
{
    //该Seg_sum_bk存在seg的哪一个block里面
    u_int bk_no;        
    Seg_sum_entry * seg_sum_entry;

    //之后加入uid用于判断一个seg的某个block是否alive
    //??

}Seg_sum_bk;

//Suppose only 1 Seg_sum_bk as Begin_bk in Seg
typedef struct Begin_bk
{
    u_int bk_no;        //Begin block在log seg的哪一个block里面
    
    //Note: Seg_sum_bk starts at the 2nd bk of seg
    Seg_sum_bk ssum_bk; 
}Begin_bk;

//----segment definition----------------
typedef struct Seg
{
    u_int seg_no;
    Begin_bk begin_bk;
    Block * bk;

    struct Seg * next;

}Seg;

typedef struct LogAddress
{
    u_int seg_no;
    u_int bk_no;

}LogAddress;

//for cleaning policy,record segment usage table
//per segment linked to seg_usage_table
//检查每一个seg，知道其利用率
typedef struct Seg_usage_table
{
    u_int seg_no;
    u_int num_live_bk;
    //u_int num_live_bytes; // should not be num of live blocks???
    time_t modify_Time;
}Seg_usage_table;


//--------1.addrs of all blocks in the inode map - ifile_no
//---- ifile contains this info------------------
//--------2.seg usage table
//--------3. current time
//--------4. pointer to the last segment written
typedef struct Checkpoint_region
{
    u_int ifile_ino;
    Seg_usage_table *seg_usage_table;
    u_int curr_time;
    Seg * last_seg_written;
}Checkpoint;


/*  ---------------- in file.h--------------------------------
removed
--------------------------------------------------------------*/

//super log segment 存整个log的信息和checkpoint等
typedef struct Super_log_seg
{
    u_int seg_no;

    u_int seg_num;      
    u_int seg_size;
    u_int bk_size;

    //----一个block可以真实存的bytes的大小
    //---- in bytes----------------------
    u_int bk_content_size;
    
    Seg_usage_table * seg_usage_table;
    /* 
     * edited 1 weng 
     * checkpoint
     * Flash
     */
    //??checkpoint还没定义
    Checkpoint * checkpoint;

//    Flash *flash;

    //points to next log seg
    Seg *next;  

}Super_log_seg;


//大小以flash memory的seg为单位
typedef struct Disk_cache
{
    u_int cache_no;

    //this cache is from which seg
    Seg * seg;

    u_int IS_JUST_UPDATE;

    struct Disk_cache * next;

}Disk_cache;


//    Flash *flash;

//-------------------method------------------------------------//


//--------create cache------------------------
//return 0: create successfully
//retrun 1: create not successfully
int create_cache();

//read_cache
//--return true all data is in cache, read directly from cache--------
//--return false not all data is in cache, read from disk,then---------
int read_cache(LogAddress logAddress, u_int length, void * buffer);


/*
 *logAddress indicates the log segment and block number within the segment, 
 *inum is the inode number of the file, 
 *block is the block number within the file,  
 *length is the number of bytes to read/write/free,  
 *buffer contains the data to read or write.
 */


//在flash memory里面分配几个segment，用来存log的结构
//调用flash.h的函数
int Log_Create();

//-----------------------------------------------------------
//input: disk 地址，返回长度为length的dis数据于buffer中
int Log_Read(LogAddress logAddr, u_int length, void * buffer);


//-----------------------------------------------------------------
//将文件的inum(inode)的第block号块写入log,写入内容
//----------------------input--------------------------------------
//--------input: block - bk_no within the file
int Log_Write(u_int inum, u_int block, u_int length,
         void * buffer, LogAddress logaddr);

//--------------------------------------------------------------------
//释放log中从logAddress开始长度为length的数据,
//--free 最小blocks数 which cover the length
int Log_Free(LogAddress logAddress, u_int length);

#endif

    

