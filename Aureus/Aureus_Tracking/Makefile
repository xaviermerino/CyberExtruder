.SUFFIXES:
#==============================================================



#==============================================================
# ARM targets
#==============================================================
.PHONY : arm
arm:
	$(MAKE) -f ./Makefile.arm all

.PHONY : clean_arm
clean_arm:
	$(MAKE) -f ./Makefile.arm clean
#==============================================================



#==============================================================
# LINUX x86 x64 targets
#==============================================================
.PHONY : linux
linux:
	$(MAKE) -f ./Makefile.linux all

.PHONY : clean_linux
clean_linux:
	$(MAKE) -f ./Makefile.linux clean
#==============================================================


#==============================================================
# CentOS x86 x64 targets
#==============================================================
.PHONY : centos
centos:
	$(MAKE) -f ./Makefile.centos all

.PHONY : clean_centos
centos_linux:
	$(MAKE) -f ./Makefile.centos clean
#==============================================================





.KEEPSTATE:
all:
	@echo "use on of the frollowing:"
	@echo "make linux"
	@echo "or:"
	@echo "make clean_linux"
	@echo "or:"
	@echo "make centos"
	@echo "or:"
	@echo "make clean_centos"
	@echo "make arm"
	@echo "or:"
	@echo "make clean_arm"	

#==============================================================

