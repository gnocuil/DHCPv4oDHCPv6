SUBDIRS = . cio tests

python_PYTHON =  __init__.py process.py socketserver_mixin.py file.py
python_PYTHON += address_formatter.py

pythondir = $(pyexecdir)/isc/util

CLEANDIRS = __pycache__

clean-local:
	rm -rf $(CLEANDIRS)
