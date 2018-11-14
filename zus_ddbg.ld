SECTIONS
{
	zus_ddbg : {
		__start_zus_ddbg = .;
		*(zus_ddbg*)
		__stop_zus_ddbg = .;
	}
}
INSERT AFTER .text;
