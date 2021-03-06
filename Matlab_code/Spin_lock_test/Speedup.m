function [ R ] = Speedup(CRate)
%UNTITLED3 Summary of this function goes here
%Detailed explanation goes here
a=10000;
s(1)=10;
c(1)=s(1);
for i=1:30 
    s(i+1)=s(i)+10;
    c(i+1)=s(i+1)*CRate;
end

param = parcluster('local');
param.NumWorkers = 4;
parpool(param, param.NumWorkers);
for i=1:31
    parfor j=0:64
        R(i,j+1)=0;
        for k=0:j;
            R(i,j+1)=R(i,j+1)+k*Pk(a, s(i), c(i), k, j);
        end
        R(i,j+1)=j-R(i,j+1);
    end
    i
end
delete(gcp)
end

function [ S ] = SingleSpeedup(a, s, c, n)
%UNTITLED3 Summary of this function goes here
% Detailed explanation goes here
for i=0:n
    S(i+1)=0;
    for k=0:i;
        S(i+1)=S(i+1)+k*Pk(a, s, c, k, i);
    end
    S(i+1)=i-S(i+1);
end
end

function [ R ] = Pk(a, s, c, k, n)
%UNTITLED3 Summary of this function goes here
% Detailed explanation goes here
R=Single(a, s, c, k, n)/PAll(a, s, c, n);
end

function [ P ] = PAll(a, s, c, n)
%UNTITLED3 Summary of this function goes here
% Detailed explanation goes here
P=0;
for i=0:n
    P=P+Single(a, s, c, i, n);
end
end

function [ R ] = Single(a, s, c, k, n)
%UNTITLED3 Summary of this function goes here
% Detailed explanation goes here
R=prod(s+(1:k)*c)/((a^k)*factorial(n-k));
end
